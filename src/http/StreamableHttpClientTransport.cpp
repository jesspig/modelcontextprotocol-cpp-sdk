// StreamableHttpClientTransport.cpp - Streamable HTTP client transport (Win32 WinHTTP / POSIX self-hosted)

#include <mcp/detail/SseEventParser.hpp>
#include <mcp/detail/StringUtils.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/Log.hpp>
#include <mcp/Methods.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/transport/detail/Url.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include "McpParamHeaders.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
// Windows.h defines GetObject macro which conflicts with JsonValue::GetObject
#pragma push_macro("GetObject")
#undef GetObject
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// Shared listen-stream state machine & SSE block parsing (both platforms)
// ═══════════════════════════════════════════════════════════════════════
namespace streamable_http_client_impl {

constexpr const char* kInitializedNotificationMethod = "notifications/initialized";
constexpr const char* kInitializeMethod = "initialize";
constexpr const char* kModernProtocolVersion = "2026-07-28";

std::string EffectiveProtocolVersion(const std::string& negotiated_version) {
    return negotiated_version.empty() ? std::string{kModernProtocolVersion}
                                      : negotiated_version;
}

std::optional<std::string> ProtocolVersionHeaderFor(
    const std::string& method, const std::string& negotiated_version) {
    if (method == kInitializeMethod) return std::nullopt;
    return EffectiveProtocolVersion(negotiated_version);
}

std::optional<std::string> SessionIdHeaderFor(
    const std::string& method, const std::string& session_id) {
    if (method == kInitializeMethod) return std::nullopt;
    if (session_id.empty()) return std::nullopt;
    return session_id;
}

JsonRpcMessage MakeSessionExpiredError(const JsonValue& request_body) {
    JsonRpcErrorResponse err;
    if (auto* id = request_body.Find("id"); id && !id->IsNull()) {
        err.id = RequestIdFromJson(*id);
    }
    err.error.code = McpErrorCode::SessionExpired;
    err.error.message = "session expired (HTTP 404)";
    return JsonRpcMessage(std::move(err));
}

std::optional<std::string> NegotiatedVersionFromResponse(
    const std::string& response_json) {
    try {
        auto jv = JsonValue::Parse(response_json);
        auto* result = jv.Find("result");
        if (!result || !result->IsObject()) return std::nullopt;
        auto* version = result->Find("protocolVersion");
        if (!version || !version->IsString()) return std::nullopt;
        return version->GetString();
    } catch (...) {
        return std::nullopt;
    }
}

enum class ListenState { Idle, Connecting, Streaming, Unsupported, GivenUp };

struct SseBlockParseResult {
    std::string data;
    std::string event_id;
    std::optional<JsonRpcMessage> message;
};

SseBlockParseResult ParseSseBlock(const std::string& block) {
    SseBlockParseResult result;
    detail::ForEachSseLine(block, [&result](std::string_view line) {
        detail::SseFieldLine field;
        if (!detail::ParseSseFieldLine(line, field)) return;
        if (field.name == "data") {
            detail::AppendSseData(result.data, field.value);
        } else if (field.name == "id") {
            result.event_id = std::string(field.value);
        }
    });
    if (!result.data.empty()) {
        try {
            result.message = DeserializeMessage(result.data);
        } catch (...) {
            result.message.reset();
        }
    }
    return result;
}

ListenState ListenStateForStatusCode(int status_code) {
    if (status_code == 200) return ListenState::Streaming;
    if (status_code == 405) return ListenState::Unsupported;
    return ListenState::Connecting;
}

inline constexpr std::chrono::milliseconds kListenRetryInitialDelay{1000};
inline constexpr std::chrono::milliseconds kListenRetryMaxDelay{30000};
inline constexpr int kMaxListenReconnectAttempts = 5;
inline constexpr std::chrono::seconds kListenStreamTimeout{600};

// 运行期间可被 Close 打断的退避睡眠：running 转 false 时立即返回。
inline void SleepInterruptibly(const std::atomic<bool>& running,
                               std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

inline constexpr std::chrono::milliseconds kParamRefreshTimeout{30000};

// SEP-2243 HeaderMismatch: the server rejected Mcp-Param-* validation.
inline bool IsHeaderMismatchBody(const std::string& body) {
    if (body.empty()) return false;
    try {
        auto parsed = JsonValue::Parse(body);
        const JsonValue* error = parsed.Find("error");
        if (!error || !error->IsObject()) return false;
        const JsonValue* code = error->Find("code");
        return code && code->IsInt() &&
               code->GetInt() == static_cast<int64_t>(McpErrorCode::HeaderMismatch);
    } catch (...) {
        return false;
    }
}

// Re-reads the tool inputSchema cache after a HeaderMismatch rejection so the
// retried request carries headers matching the server's current schema.
inline bool RefreshToolAnnotations(const std::string& endpoint,
                                   const std::string& session_id,
                                   const std::string& protocol_version,
                                   const std::string& auth_value,
                                   const std::map<std::string, std::string>& additional_headers,
                                   http_detail::ToolAnnotationCache& cache) {
    detail::net::HttpRequestSpec req;
    req.method = "POST";
    req.url = endpoint;
    req.timeout = kParamRefreshTimeout;
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json, text/event-stream";
    req.headers["Mcp-Method"] = std::string(methods::kListTools);
    if (!session_id.empty()) req.headers["Mcp-Session-Id"] = session_id;
    if (!protocol_version.empty()) req.headers["MCP-Protocol-Version"] = protocol_version;
    for (const auto& [header_name, header_value] : additional_headers) {
        req.headers[header_name] = header_value;
    }
    if (!auth_value.empty()) req.headers["Authorization"] = auth_value;
    req.body = R"({"jsonrpc":"2.0","id":0,"method":"tools/list"})";
    try {
        detail::net::HttpClient client;
        auto resp = client.Request(req);
        if (resp.status_code != 200 || resp.body.empty()) return false;
        JsonRpcMessage message;
        if (resp.body.front() == '{') {
            message = DeserializeMessage(resp.body);
        } else {
            auto block = ParseSseBlock(resp.body);
            if (!block.message) return false;
            message = std::move(*block.message);
        }
        auto* response = std::get_if<JsonRpcResponse>(&message);
        if (!response) return false;
        cache.ObserveToolsListResult(response->result);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace streamable_http_client_impl

// ═══════════════════════════════════════════════════════════════════════
// Win32 implementation (WinHTTP)
// ═══════════════════════════════════════════════════════════════════════
#ifdef _WIN32
namespace {

constexpr DWORD kHttpTimeoutMs = 30000;

std::wstring ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), &w[0], len);
    return w;
}

// ── Win32 StreamableHttpSessionTransport ──
class StreamableHttpSessionTransport : public TransportBase {
public:
    StreamableHttpSessionTransport(
        HttpClientTransportOptions options)
        : TransportBase()
        , options_(std::move(options))
    {
        current_session_id_ = options_.known_session_id;
    }

    ~StreamableHttpSessionTransport() override { Close(); }

    void Start() override {
        if (running_.exchange(true)) return;
        send_thread_ = std::thread([this] {
            detail::SetThreadName("mcp-worker");
            SendLoop();
        });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            delete_pending_ = true;
            send_cv_.notify_one();
        }
        // Stop the listen stream first: interrupt its in-flight request (same
        // pattern as sse_request_) and join its thread before tearing down the
        // POST path, so Close cannot hang on a blocked GET read.
        auto listen_req = listen_request_.exchange(nullptr);
        if (listen_req) {
            WinHttpSetTimeouts(listen_req, 0, 0, 0, 500);
        }
        std::thread listen_thread;
        {
            std::lock_guard<std::mutex> lk(listen_mutex_);
            listen_thread = std::move(listen_thread_);
        }
        detail::JoinThreadSafely(listen_thread);
        // Interrupt a blocked WinHttpReadData (SSE POST response) so the send
        // thread can exit promptly. sse_request_ names the in-flight request
        // handle; Close only touches it to shorten its receive timeout.
        auto sse_req = sse_request_.exchange(nullptr);
        if (sse_req) {
            WinHttpSetTimeouts(sse_req, 0, 0, 0, 500);
        }
        detail::JoinThreadSafely(send_thread_);
        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;
        MaybeStartListenStream(message);
        bool is_request = IsRequest(message);
        auto body = SerializeMessage(std::move(message));
        if (is_request) {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_queue_.push(std::move(body));
        } else {
            // Notifications/responses carry no response-awaiting semantics;
            // posting them immediately cannot wait behind an in-flight
            // request POST, which would deadlock server→client requests
            // (e.g. elicitation completing a suspended tools/call).
            LaunchImmediatePost(std::move(body));
            return;
        }
        send_cv_.notify_one();
    }

private:
    // Fire-and-forget POST on a short-lived detached thread. The thread keeps
    // the session alive via shared_from_this, so a detached thread outliving
    // Close() never touches a destroyed object; DoPost's HTTP timeouts bound
    // the thread's lifetime.
    void LaunchImmediatePost(std::string body) {
        try {
            auto self = std::static_pointer_cast<StreamableHttpSessionTransport>(
                shared_from_this());
            std::thread([self, body = std::move(body)]() mutable {
                detail::SetThreadName("mcp-post");
                try {
                    self->DoPost(body);
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("immediate POST failed: ") + e.what());
                } catch (...) {
                    MCP_LOG(Error, "immediate POST failed");
                }
            }).detach();
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("immediate POST launch failed: ") + e.what());
        }
    }
    void MaybeStartListenStream(const JsonRpcMessage& message) {
        if (!options_.enable_listen_stream) return;
        auto* notification = AsNotification(message);
        if (!notification || notification->method !=
            streamable_http_client_impl::kInitializedNotificationMethod) {
            return;
        }
        if (listen_state_.load() !=
            streamable_http_client_impl::ListenState::Idle) {
            return;
        }
        std::lock_guard<std::mutex> lk(listen_mutex_);
        if (!running_) return;
        if (listen_requested_.exchange(true)) return;
        listen_thread_ = std::thread([this] {
            detail::SetThreadName("mcp-listen");
            listen_state_ = streamable_http_client_impl::ListenState::Connecting;
            RunListenStream();
        });
    }

    void RunListenStream() {
        using streamable_http_client_impl::ListenState;
        int reconnects = 0;
        auto delay = streamable_http_client_impl::kListenRetryInitialDelay;
        for (;;) {
            if (!running_) return;
            auto result = DoListenGet();
            if (result == ListenState::Unsupported) {
                listen_state_ = ListenState::Unsupported;
                return;
            }
            if (reconnects >= streamable_http_client_impl::kMaxListenReconnectAttempts) {
                listen_state_ = ListenState::GivenUp;
                return;
            }
            ++reconnects;
            streamable_http_client_impl::SleepInterruptibly(running_, delay);
            delay = (std::min)(delay * 2, streamable_http_client_impl::kListenRetryMaxDelay);
        }
    }

    streamable_http_client_impl::ListenState DoListenGet() {
        using streamable_http_client_impl::ListenState;
        HINTERNET hSession = WinHttpOpen(L"MCP-HTTP-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return ListenState::Connecting;

        auto url = detail::ParseUrl(options_.endpoint);
        auto wh = ToWideStr(url.host);
        auto wp = ToWideStr(url.path);
        if (wp.empty()) wp = L"/";

        HINTERNET hConnect = WinHttpConnect(hSession, wh.c_str(), url.port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return ListenState::Connecting;
        }

        DWORD flags = (url.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
            wp.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return ListenState::Connecting;
        }
        auto recv_ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                streamable_http_client_impl::kListenStreamTimeout).count());
        WinHttpSetTimeouts(hRequest, kHttpTimeoutMs, kHttpTimeoutMs,
                           kHttpTimeoutMs, recv_ms);

        std::wstring hdrs = L"Accept: text/event-stream\r\n";
        hdrs += L"MCP-Protocol-Version: " + ToWideStr(
            streamable_http_client_impl::EffectiveProtocolVersion(
                NegotiatedVersion())) + L"\r\n";
        auto listen_sid = CurrentSessionId();
        if (!listen_sid.empty()) {
            hdrs += L"Mcp-Session-Id: " +
                ToWideStr(listen_sid) + L"\r\n";
        }
        {
            std::lock_guard<std::mutex> lk(last_event_id_mutex_);
            if (!last_event_id_.empty()) {
                hdrs += L"Last-Event-ID: " + ToWideStr(last_event_id_) + L"\r\n";
            }
        }
        WinHttpAddRequestHeaders(hRequest, hdrs.data(),
            static_cast<DWORD>(hdrs.size()), WINHTTP_ADDREQ_FLAG_ADD);

        auto finish = [&](ListenState state) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return state;
        };

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            return finish(ListenState::Connecting);
        }

        DWORD status_code = 0;
        DWORD scSize = sizeof(status_code);
        BOOL status_ok = WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                nullptr, &status_code, &scSize, nullptr);
        auto mapped = streamable_http_client_impl::ListenStateForStatusCode(
            status_ok ? static_cast<int>(status_code) : 0);
        if (mapped != ListenState::Streaming) {
            return finish(mapped);
        }
        wchar_t contentType[64] = {};
        DWORD ctSize = sizeof(contentType);
        bool isSse = false;
        if (WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_CONTENT_TYPE, nullptr,
                contentType, &ctSize, nullptr)) {
            isSse = (wcsstr(contentType, L"text/event-stream") != nullptr);
        }
        if (!isSse) {
            return finish(ListenState::Connecting);
        }

        listen_state_ = ListenState::Streaming;
        listen_request_ = hRequest;
        if (!running_) {
            listen_request_ = nullptr;
            return finish(ListenState::Streaming);
        }
        std::string pending;
        char buf[4096];
        for (;;) {
            if (!running_) break;
            DWORD navail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &navail) || navail == 0)
                break;
            DWORD nread = 0;
            DWORD nwant = navail < sizeof(buf) ? navail : static_cast<DWORD>(sizeof(buf));
            if (!WinHttpReadData(hRequest, buf, nwant, &nread) || nread == 0)
                break;
            pending.append(buf, nread);
            if (pending.size() > detail::kMaxMessageSize) {
                MCP_LOG(Error, "Listen SSE stream exceeded max message size");
                break;
            }
            size_t pos;
            bool channel_closed = false;
            while ((pos = pending.find("\n\n")) != std::string::npos) {
                std::string block = pending.substr(0, pos);
                pending.erase(0, pos + 2);
                if (!ProcessListenBlock(block)) {
                    channel_closed = true;
                    break;
                }
            }
            if (channel_closed) break;
        }
        listen_request_ = nullptr;
        return finish(ListenState::Streaming);
    }

    bool ProcessListenBlock(const std::string& block) {
        auto parsed = streamable_http_client_impl::ParseSseBlock(block);
        if (!parsed.event_id.empty()) {
            std::lock_guard<std::mutex> lk(last_event_id_mutex_);
            last_event_id_ = parsed.event_id;
        }
        if (parsed.data.empty()) return true;
        if (parsed.data.size() > detail::kMaxMessageSize) {
            MCP_LOG(Error, "HTTP SSE block exceeded max message size");
            return true;
        }
        if (parsed.message && channel_) {
            return channel_->Send(std::move(*parsed.message));
        }
        return true;
    }

    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lk(send_mutex_);
                send_cv_.wait(lk, [this] {
                    return !send_queue_.empty() || !running_;
                });
                if (!running_) break;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }
            DoPost(body);
        }
        if (delete_pending_.exchange(false)) {
            DoDelete();
        }
    }

    void DoDelete() {
        auto sid = CurrentSessionId();
        if (sid.empty()) return;
        HINTERNET hSession = WinHttpOpen(L"MCP-HTTP-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        auto url = detail::ParseUrl(options_.endpoint);
        auto wh = ToWideStr(url.host);
        auto wp = ToWideStr(url.path);
        if (wp.empty()) wp = L"/";

        HINTERNET hConnect = WinHttpConnect(hSession, wh.c_str(), url.port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return;
        }

        DWORD flags = (url.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"DELETE",
            wp.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return;
        }

        std::wstring hdrs = L"Mcp-Session-Id: " +
            ToWideStr(sid) + L"\r\n";
        WinHttpAddRequestHeaders(hRequest, hdrs.data(),
            static_cast<DWORD>(hdrs.size()), WINHTTP_ADDREQ_FLAG_ADD);

        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            char buf[4096];
            DWORD read = 0;
            while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                read = 0;
            }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
    }

    void DoPost(const std::string& body) {
        auto wide_to_utf8 = [](const wchar_t* w) -> std::string {
            if (!w) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len <= 0) return {};
            std::string s(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
            return s;
        };
        std::string auth_value;
        for (int attempt = 0; attempt < 2; ++attempt) {
            HINTERNET hSession = WinHttpOpen(L"MCP-HTTP-Client/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSession) {
                MCP_LOG(Error, "WinHttpOpen failed");
                NotifyError("WinHttpOpen failed");
                return;
            }

            auto url = detail::ParseUrl(options_.endpoint);
            auto wh = ToWideStr(url.host);
            auto wp = ToWideStr(url.path);
            if (wp.empty()) wp = L"/";

            HINTERNET hConnect = WinHttpConnect(hSession, wh.c_str(), url.port, 0);
            if (!hConnect) {
                MCP_LOG(Error, "WinHttpConnect failed");
                NotifyError("WinHttpConnect failed");
                WinHttpCloseHandle(hSession);
                return;
            }

            DWORD flags = (url.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
                wp.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!hRequest) {
                MCP_LOG(Error, "WinHttpOpenRequest failed");
                NotifyError("WinHttpOpenRequest failed");
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }
            if (!WinHttpSetTimeouts(hRequest, kHttpTimeoutMs, kHttpTimeoutMs,
                                    kHttpTimeoutMs, kHttpTimeoutMs)) {
                MCP_LOG(Warning, "WinHttpSetTimeouts failed");
            }

            // Headers per MCP Streamable HTTP spec
            std::wstring hdrs = L"Content-Type: application/json\r\n"
                                L"Accept: application/json, text/event-stream\r\n";
            std::string method;
            try {
                auto body_jv2 = JsonValue::Parse(body);
                if (auto* m = body_jv2.Find("method"); m && m->IsString()) {
                    method = m->GetString();
                    hdrs += L"Mcp-Method: " + ToWideStr(method) + L"\r\n";
                }
                if (auto* p = body_jv2.Find("params"); p && p->IsObject()) {
                    const auto& obj = p->GetObject();
                    if (auto n = obj.find("name"); n != obj.end() && n->second.IsString()) {
                        hdrs += L"Mcp-Name: " + ToWideStr(McpHeaderValue(n->second)) + L"\r\n";
                    } else if (auto u = obj.find("uri"); u != obj.end() && u->second.IsString()) {
                        hdrs += L"Mcp-Name: " + ToWideStr(McpHeaderValue(u->second)) + L"\r\n";
                    }
                }
                if (method == "tools/list") {
                    NoteToolsListRequest(body_jv2);
                } else if (method == "tools/call") {
                    AppendParamHeaders(hdrs, body_jv2, method);
                }
            } catch (...) {
                MCP_LOG(Warning, "HTTP header parse failed");
                hdrs += L"Mcp-Method: tools/call\r\n";
            }
            bool is_initialize =
                method == streamable_http_client_impl::kInitializeMethod;
            if (auto sid = streamable_http_client_impl::SessionIdHeaderFor(
                    method, CurrentSessionId())) {
                hdrs += L"Mcp-Session-Id: " + ToWideStr(*sid) + L"\r\n";
            }
            if (auto version = streamable_http_client_impl::ProtocolVersionHeaderFor(
                    method, NegotiatedVersion())) {
                hdrs += L"MCP-Protocol-Version: " + ToWideStr(*version) + L"\r\n";
            }
            for (auto& [k, v] : options_.additional_headers) {
                auto wk = ToWideStr(k);
                auto wv = ToWideStr(v);
                hdrs += wk + L": " + wv + L"\r\n";
            }
            if (!auth_value.empty()) {
                hdrs += L"Authorization: " + ToWideStr(auth_value) + L"\r\n";
            }

            WinHttpAddRequestHeaders(hRequest, hdrs.data(),
                static_cast<DWORD>(hdrs.size()), WINHTTP_ADDREQ_FLAG_ADD);

            BOOL sent = WinHttpSendRequest(hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), 0);

            if (!sent) {
                MCP_LOG(Error, "WinHttpSendRequest failed");
                NotifyError("WinHttpSendRequest failed");
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }
            if (!WinHttpReceiveResponse(hRequest, nullptr)) {
                MCP_LOG(Error, "WinHttpReceiveResponse failed");
                NotifyError("WinHttpReceiveResponse failed");
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }

            // Check response status code; a 4xx body may still be a JSON-RPC error payload
            DWORD status_code = 0;
            DWORD scSize = sizeof(status_code);
            BOOL status_ok = WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    nullptr, &status_code, &scSize, nullptr);

            // Capture a session id from any response; later requests carry it.
            wchar_t sid_buf[512] = {};
            DWORD sid_size = sizeof(sid_buf);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM,
                    L"Mcp-Session-Id", sid_buf, &sid_size, nullptr)) {
                StoreSessionId(wide_to_utf8(sid_buf));
            }

            if (status_ok && status_code >= 400)
            {
                if ((status_code == 401 || status_code == 403) && attempt == 0 &&
                    options_.auth_challenge_handler) {
                    wchar_t www_auth[512] = {};
                    DWORD waSize = sizeof(www_auth);
                    std::string www_auth_str;
                    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM,
                            L"WWW-Authenticate", www_auth, &waSize, nullptr)) {
                        www_auth_str = wide_to_utf8(www_auth);
                    }
                    auto new_auth = options_.auth_challenge_handler(www_auth_str);
                    if (!new_auth.empty()) {
                        auth_value = std::move(new_auth);
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        continue;
                    }
                }
                // A 4xx body may carry a JSON-RPC error response (e.g. -32601
                // mapped to HTTP 404); deliver it to the channel instead of
                // failing the connection.
                std::string err_body;
                char ebuf[4096];
                DWORD eread = 0;
                while (WinHttpReadData(hRequest, ebuf, sizeof(ebuf), &eread) && eread > 0) {
                    err_body.append(ebuf, eread);
                    eread = 0;
                }
                if (status_code == 400 && attempt == 0 &&
                    streamable_http_client_impl::IsHeaderMismatchBody(err_body)) {
                    if (streamable_http_client_impl::RefreshToolAnnotations(
                            options_.endpoint, CurrentSessionId(),
                            NegotiatedVersion(), auth_value,
                            options_.additional_headers, tool_annotations_)) {
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        continue;
                    }
                }
                bool delivered = false;
                if (!err_body.empty() && err_body.size() <= detail::kMaxMessageSize) {
                    try {
                        JsonRpcMessage msg = DeserializeMessage(err_body);
                        DeliverIncoming(std::move(msg));
                        delivered = true;
                    } catch (...) {
                    }
                }
                if (!delivered && status_code == 404) {
                    try {
                        auto req_body = JsonValue::Parse(body);
                        if (channel_) {
                            channel_->Send(streamable_http_client_impl::
                                MakeSessionExpiredError(req_body));
                            delivered = true;
                        }
                    } catch (...) {
                    }
                }
                if (!delivered) {
                    MCP_LOG(Error, std::string("HTTP POST returned status ") + std::to_string(status_code));
                    NotifyError("HTTP POST returned status " + std::to_string(status_code));
                }
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }

            // 202 acknowledges a notification; it never carries a response.
            if (status_ok && status_code == 202)
            {
                bool had_id = false;
                try {
                    auto jv = JsonValue::Parse(body);
                    had_id = jv.Contains("id");
                } catch (...) {
                }
                if (had_id) {
                    MCP_LOG(Warning, "HTTP POST returned 202 for a request; response lost");
                } else {
                    MCP_LOG(Info, "HTTP POST accepted (202)");
                }
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }

            // Read response headers to determine content type
            wchar_t contentType[64] = {};
            DWORD ctSize = sizeof(contentType);
            bool isSse = false;
            if (WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_CONTENT_TYPE, nullptr,
                    contentType, &ctSize, nullptr)) {
                isSse = (wcsstr(contentType, L"text/event-stream") != nullptr);
            }

            if (isSse) {
                // POST SSE response stream: dispatch each complete event block
                // as its bytes arrive; a held-open stream (server→client request
                // awaiting our reply) must not defer dispatch until EOF.
                // QueryDataAvailable + ReadData(available) is the documented
                // incremental read pairing: bare WinHttpReadData would wait to
                // fill the whole 4KB buffer before returning.
                sse_request_ = hRequest;
                std::string sse_body;
                char sbuf[4096];
                for (;;) {
                    DWORD savail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &savail) || savail == 0)
                        break;
                    DWORD sread = 0;
                    DWORD swant = savail < sizeof(sbuf) ? savail
                                                        : static_cast<DWORD>(sizeof(sbuf));
                    if (!WinHttpReadData(hRequest, sbuf, swant, &sread) || sread == 0)
                        break;
                    sse_body.append(sbuf, sread);
                    if (sse_body.size() > detail::kMaxMessageSize) {
                        sse_request_ = nullptr;
                        sse_body.clear();
                        MCP_LOG(Error, "HTTP SSE response exceeded max message size");
                        NotifyError("HTTP SSE response exceeded max message size");
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return;
                    }
                    size_t pos;
                    while ((pos = sse_body.find("\n\n")) != std::string::npos) {
                        std::string block = sse_body.substr(0, pos);
                        sse_body.erase(0, pos + 2);
                        DispatchSseBlock(block, is_initialize);
                    }
                }
                sse_request_ = nullptr;
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            } else {
                // Drain response (single JSON response)
                std::string resp_body;
                char buf[4096];
                DWORD read = 0;
                while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                    resp_body.append(buf, read);
                    read = 0;
                }
                // Try to parse as JSON-RPC response and enqueue
                if (!resp_body.empty()) {
                    if (resp_body.size() > detail::kMaxMessageSize) {
                        MCP_LOG(Error, "HTTP response exceeded max message size");
                        NotifyError("HTTP response exceeded max message size");
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return;
                    }
                    if (is_initialize) {
                        if (auto v = streamable_http_client_impl::NegotiatedVersionFromResponse(
                                resp_body)) {
                            StoreNegotiatedVersion(std::move(*v));
                        }
                    }
                    try {
                        JsonRpcMessage msg = DeserializeMessage(resp_body);
                        DeliverIncoming(std::move(msg));
                    } catch (...) { MCP_LOG(Error, "HTTP response parse failed"); }
                }
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return;
            }
        }
        // Both attempts failed with 401/403.
    }

    void DispatchSseBlock(const std::string& block, bool learn_version) {
        auto parsed = streamable_http_client_impl::ParseSseBlock(block);
        if (parsed.data.empty()) return;
        if (parsed.data.size() > detail::kMaxMessageSize) {
            MCP_LOG(Error, "HTTP SSE block exceeded max message size");
            NotifyError("HTTP SSE block exceeded max message size");
            return;
        }
        if (learn_version && parsed.message) {
            if (auto v = streamable_http_client_impl::NegotiatedVersionFromResponse(
                    parsed.data)) {
                StoreNegotiatedVersion(std::move(*v));
            }
        }
        if (parsed.message) {
            DeliverIncoming(std::move(*parsed.message));
        } else {
            MCP_LOG(Error, "HTTP SSE block parse failed");
        }
    }

    std::string NegotiatedVersion() {
        std::lock_guard<std::mutex> lk(version_mutex_);
        return negotiated_version_;
    }

    std::string CurrentSessionId() {
        std::lock_guard<std::mutex> lk(session_id_mutex_);
        return current_session_id_;
    }

    void StoreSessionId(std::string sid) {
        std::lock_guard<std::mutex> lk(session_id_mutex_);
        current_session_id_ = std::move(sid);
    }

    void StoreNegotiatedVersion(std::string version) {
        std::lock_guard<std::mutex> lk(version_mutex_);
        if (negotiated_version_.empty()) negotiated_version_ = std::move(version);
    }

    bool DeliverIncoming(JsonRpcMessage message) {
        ObserveToolsListResult(message);
        return channel_ && channel_->Send(std::move(message));
    }

    void ObserveToolsListResult(JsonRpcMessage& message) {
        auto* response = std::get_if<JsonRpcResponse>(&message);
        if (!response) return;
        const std::string key = http_detail::KeyFromId(response->id);
        if (!tool_annotations_.ConsumeToolsListRequest(key)) return;
        auto rejected = tool_annotations_.ObserveToolsListResult(response->result);
        for (const auto& name : rejected) {
            MCP_LOG(Warning, "rejecting tool with invalid x-mcp-header: " + name);
        }
    }

    static std::string McpHeaderValue(const JsonValue& value) {
        return http_detail::EncodeHeaderValue(value).value_or(std::string());
    }

    void NoteToolsListRequest(const JsonValue& body) {
        const JsonValue* id = body.Find("id");
        if (!id) return;
        if (id->IsInt()) tool_annotations_.NoteToolsListRequest(std::to_string(id->GetInt()));
        else if (id->IsString()) tool_annotations_.NoteToolsListRequest(id->GetString());
    }

    void AppendParamHeaders(std::wstring& hdrs, const JsonValue& body,
                            const std::string& method) {
        const JsonValue* params = body.Find("params");
        if (!params || !params->IsObject()) return;
        for (const auto& [name, value] : tool_annotations_.BuildParamHeaders(method, *params)) {
            hdrs += ToWideStr(name) + L": " + ToWideStr(value) + L"\r\n";
        }
    }

    HttpClientTransportOptions options_;
    std::mutex session_id_mutex_;
    std::string current_session_id_;
    std::mutex version_mutex_;
    std::string negotiated_version_;
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> delete_pending_{false};
    // Handle of the request whose SSE response is being read by the send
    // thread; Close() shortens its receive timeout to interrupt the read.
    std::atomic<HINTERNET> sse_request_{nullptr};
    std::thread listen_thread_;
    std::mutex listen_mutex_;
    std::atomic<streamable_http_client_impl::ListenState> listen_state_{
        streamable_http_client_impl::ListenState::Idle};
    std::mutex last_event_id_mutex_;
    std::string last_event_id_;
    std::atomic<bool> listen_requested_{false};
    // Handle of the in-flight listen GET request; Close() shortens its
    // receive timeout to interrupt the read (same pattern as sse_request_).
    std::atomic<HINTERNET> listen_request_{nullptr};

    http_detail::ToolAnnotationCache tool_annotations_;
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// POSIX implementation using the internal HTTP client
// ═══════════════════════════════════════════════════════════════════════
#else

namespace httpclient_posix_impl {

std::string GetHeader(const detail::net::HttpResponseInfo& resp,
                      std::string_view name)
{
    std::string lower = detail::ToLower(name);
    auto it = resp.headers.find(lower);
    return it == resp.headers.end() ? std::string{} : it->second;
}

} // namespace httpclient_posix_impl

namespace {

constexpr int kHttpRequestTimeoutSeconds = 30;

struct ListenAbort {};

struct PostBodyTooLarge {};

class StreamableHttpSessionTransport : public TransportBase {
public:
    explicit StreamableHttpSessionTransport(
        HttpClientTransportOptions options)
        : options_(std::move(options))
    {
        current_session_id_ = options_.known_session_id;
    }

    ~StreamableHttpSessionTransport() override { Close(); }

    void Start() override {
        if (running_.exchange(true)) return;
        send_thread_ = std::thread([this] { SendLoop(); });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            delete_pending_ = true;
            send_cv_.notify_one();
        }
        // Stop the listen stream first: interrupt its in-flight GET (closing
        // the socket wakes the blocked read) and join its thread before
        // tearing down the POST path, so Close cannot hang.
        {
            std::lock_guard<std::mutex> lk(listen_mutex_);
            if (listen_client_) listen_client_->Close();
        }
        std::thread listen_thread;
        {
            std::lock_guard<std::mutex> lk(listen_mutex_);
            listen_thread = std::move(listen_thread_);
        }
        detail::JoinThreadSafely(listen_thread);
        detail::JoinThreadSafely(send_thread_);
        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;
        MaybeStartListenStream(message);
        bool is_request = IsRequest(message);
        auto body = SerializeMessage(std::move(message));
        if (is_request) {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_queue_.push(std::move(body));
        } else {
            // Notifications/responses carry no response-awaiting semantics;
            // posting them immediately cannot wait behind an in-flight
            // request POST, which would deadlock server→client requests
            // (e.g. elicitation completing a suspended tools/call).
            LaunchImmediatePost(std::move(body));
            return;
        }
        send_cv_.notify_one();
    }

private:
    // Fire-and-forget POST on a short-lived detached thread. The thread keeps
    // the session alive via shared_from_this, so a detached thread outliving
    // Close() never touches a destroyed object; DoPost's HTTP timeouts bound
    // the thread's lifetime.
    void LaunchImmediatePost(std::string body) {
        try {
            auto self = std::static_pointer_cast<StreamableHttpSessionTransport>(
                shared_from_this());
            std::thread([self, body = std::move(body)]() mutable {
                try {
                    self->DoPost(body);
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("immediate POST failed: ") + e.what());
                } catch (...) {
                    MCP_LOG(Error, "immediate POST failed");
                }
            }).detach();
        } catch (const std::exception& e) {
            MCP_LOG(Error, std::string("immediate POST launch failed: ") + e.what());
        }
    }
    void MaybeStartListenStream(const JsonRpcMessage& message) {
        if (!options_.enable_listen_stream) return;
        auto* notification = AsNotification(message);
        if (!notification || notification->method !=
            streamable_http_client_impl::kInitializedNotificationMethod) {
            return;
        }
        if (listen_state_.load() !=
            streamable_http_client_impl::ListenState::Idle) {
            return;
        }
        std::lock_guard<std::mutex> lk(listen_mutex_);
        if (!running_) return;
        if (listen_requested_.exchange(true)) return;
        listen_thread_ = std::thread([this] {
            listen_state_ = streamable_http_client_impl::ListenState::Connecting;
            RunListenStream();
        });
    }

    void RunListenStream() {
        using streamable_http_client_impl::ListenState;
        int reconnects = 0;
        auto delay = streamable_http_client_impl::kListenRetryInitialDelay;
        for (;;) {
            if (!running_) return;
            auto result = DoListenGet();
            if (result == ListenState::Unsupported) {
                listen_state_ = ListenState::Unsupported;
                return;
            }
            if (reconnects >= streamable_http_client_impl::kMaxListenReconnectAttempts) {
                listen_state_ = ListenState::GivenUp;
                return;
            }
            ++reconnects;
            streamable_http_client_impl::SleepInterruptibly(running_, delay);
            delay = (std::min)(delay * 2, streamable_http_client_impl::kListenRetryMaxDelay);
        }
    }

    streamable_http_client_impl::ListenState DoListenGet() {
        using streamable_http_client_impl::ListenState;
        detail::net::HttpRequestSpec req;
        req.method = "GET";
        req.url = options_.endpoint;
        req.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            streamable_http_client_impl::kListenStreamTimeout);
        req.headers["Accept"] = "text/event-stream";
        req.headers["MCP-Protocol-Version"] =
            streamable_http_client_impl::EffectiveProtocolVersion(
                NegotiatedVersion());
        auto listen_sid = CurrentSessionId();
        if (!listen_sid.empty()) {
            req.headers["Mcp-Session-Id"] = std::move(listen_sid);
        }
        {
            std::lock_guard<std::mutex> lk(last_event_id_mutex_);
            if (!last_event_id_.empty()) {
                req.headers["Last-Event-ID"] = last_event_id_;
            }
        }
        auto client = std::make_unique<detail::net::HttpClient>();
        {
            std::lock_guard<std::mutex> lk(listen_mutex_);
            if (!running_) return ListenState::Connecting;
            listen_client_ = client.get();
        }
        std::string pending;
        bool stream_active = false;
        bool read_error = false;
        detail::net::HttpResponseInfo resp;
        try {
            resp = client->Request(req, [&](std::string_view chunk) {
                if (!stream_active) {
                    stream_active = true;
                    listen_state_ = ListenState::Streaming;
                }
                pending.append(chunk.data(), chunk.size());
                if (pending.size() > detail::kMaxMessageSize) {
                    MCP_LOG(Error, "Listen SSE stream exceeded max message size");
                    throw ListenAbort{};
                }
                size_t pos;
                while ((pos = pending.find("\n\n")) != std::string::npos) {
                    std::string block = pending.substr(0, pos);
                    pending.erase(0, pos + 2);
                    if (!ProcessListenBlock(block)) throw ListenAbort{};
                }
            });
        } catch (const ListenAbort&) {
        } catch (const std::exception&) {
            read_error = true;
        }
        {
            std::lock_guard<std::mutex> lk(listen_mutex_);
            listen_client_ = nullptr;
        }
        if (read_error) {
            return stream_active ? ListenState::Streaming : ListenState::Connecting;
        }
        auto mapped = streamable_http_client_impl::ListenStateForStatusCode(
            resp.status_code);
        return mapped;
    }

    bool ProcessListenBlock(const std::string& block) {
        auto parsed = streamable_http_client_impl::ParseSseBlock(block);
        if (!parsed.event_id.empty()) {
            std::lock_guard<std::mutex> lk(last_event_id_mutex_);
            last_event_id_ = parsed.event_id;
        }
        if (parsed.data.empty()) return true;
        if (parsed.data.size() > detail::kMaxMessageSize) {
            MCP_LOG(Error, "HTTP SSE block exceeded max message size");
            return true;
        }
        if (parsed.message && channel_) {
            return channel_->Send(std::move(*parsed.message));
        }
        return true;
    }

    void SendLoop() {
        while (running_) {
            std::string body;
            {
                std::unique_lock<std::mutex> lk(send_mutex_);
                send_cv_.wait(lk, [this]{ return !send_queue_.empty() || !running_; });
                if (!running_) break;
                body = std::move(send_queue_.front());
                send_queue_.pop();
            }
            DoPost(body);
        }
        if (delete_pending_.exchange(false)) {
            DoDelete();
        }
    }

    void DoDelete() {
        auto sid = CurrentSessionId();
        if (sid.empty()) return;
        detail::net::HttpRequestSpec req;
        req.method = "DELETE";
        req.url = options_.endpoint;
        req.timeout = std::chrono::milliseconds(kHttpRequestTimeoutSeconds * 1000);
        req.headers["Mcp-Session-Id"] = std::move(sid);
        try {
            detail::net::HttpClient client;
            (void)client.Request(req);
        } catch (...) {
            MCP_LOG(Error, "HTTP DELETE failed");
        }
    }

    void DoPost(const std::string& body) {
        std::string auth_value;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::unordered_map<std::string, std::string> headers;
            headers["Content-Type"] = "application/json";
            headers["Accept"] = "application/json, text/event-stream";
            std::string method;
            try {
                auto jv = JsonValue::Parse(body);
                if (auto* m = jv.Find("method"); m && m->IsString()) {
                    method = m->GetString();
                    headers["Mcp-Method"] = method;
                }
                if (auto* p = jv.Find("params"); p && p->IsObject()) {
                    const auto& obj = p->GetObject();
                    if (auto n = obj.find("name"); n != obj.end() && n->second.IsString())
                        headers["Mcp-Name"] = McpHeaderValue(n->second);
                    else if (auto u = obj.find("uri"); u != obj.end() && u->second.IsString())
                        headers["Mcp-Name"] = McpHeaderValue(u->second);
                }
                if (method == "tools/list") {
                    NoteToolsListRequest(jv);
                } else if (method == "tools/call") {
                    AppendParamHeaders(headers, jv, method);
                }
            } catch (...) {
                headers["Mcp-Method"] = "unknown";
            }
            bool is_initialize =
                method == streamable_http_client_impl::kInitializeMethod;
            if (auto sid = streamable_http_client_impl::SessionIdHeaderFor(
                    method, CurrentSessionId())) {
                headers["Mcp-Session-Id"] = *sid;
            }
            if (auto version = streamable_http_client_impl::ProtocolVersionHeaderFor(
                    method, NegotiatedVersion())) {
                headers["MCP-Protocol-Version"] = *version;
            }
            if (!auth_value.empty()) {
                headers["Authorization"] = auth_value;
            }
            for (auto& [k, v] : options_.additional_headers)
                headers[k] = v;

            detail::net::HttpRequestSpec req;
            req.method = "POST";
            req.url = options_.endpoint;
            req.body = body;
            req.timeout = std::chrono::milliseconds(kHttpRequestTimeoutSeconds * 1000);
            req.headers = headers;
            detail::net::HttpClient client;
            detail::net::HttpResponseInfo resp;
            std::string full_body;
            std::string sse_pending;
            try {
                resp = client.Request(req, [&](std::string_view chunk) {
                    full_body.append(chunk.data(), chunk.size());
                    if (full_body.size() > detail::kMaxMessageSize) {
                        throw PostBodyTooLarge{};
                    }
                    // Headers are invisible inside the callback (HttpClient
                    // fills them on return), so split unconditionally: a JSON
                    // body never yields a block whose lines start with "data:",
                    // and DispatchSseBlock silently ignores such blocks.
                    sse_pending.append(chunk.data(), chunk.size());
                    size_t pos;
                    while ((pos = sse_pending.find("\n\n")) != std::string::npos) {
                        std::string block = sse_pending.substr(0, pos);
                        sse_pending.erase(0, pos + 2);
                        DispatchSseBlock(block, is_initialize);
                    }
                });
            } catch (const PostBodyTooLarge&) {
                MCP_LOG(Error, "HTTP response exceeded max message size");
                NotifyError("HTTP response exceeded max message size");
                return;
            } catch (...) {
                MCP_LOG(Error, "HTTP POST failed");
                NotifyError("HTTP POST failed");
                return;
            }

            // Capture a session id from any response; later requests carry it.
            auto sid = httpclient_posix_impl::GetHeader(resp, "Mcp-Session-Id");
            if (!sid.empty()) {
                StoreSessionId(std::move(sid));
            }

            if (resp.status_code >= 400) {
                if ((resp.status_code == 401 || resp.status_code == 403) &&
                    attempt == 0 && options_.auth_challenge_handler) {
                    auto www_auth = httpclient_posix_impl::GetHeader(resp, "WWW-Authenticate");
                    auto new_auth = options_.auth_challenge_handler(www_auth);
                    if (!new_auth.empty()) {
                        auth_value = std::move(new_auth);
                        continue;
                    }
                }
                // A 4xx body may carry a JSON-RPC error response (e.g. -32601
                // mapped to HTTP 404); deliver it to the channel instead of
                // failing the connection.
                if (resp.status_code == 400 && attempt == 0 &&
                    streamable_http_client_impl::IsHeaderMismatchBody(full_body)) {
                    if (streamable_http_client_impl::RefreshToolAnnotations(
                            options_.endpoint, CurrentSessionId(),
                            NegotiatedVersion(), auth_value,
                            options_.additional_headers, tool_annotations_)) {
                        continue;
                    }
                }
                bool delivered = false;
                if (!full_body.empty() && full_body.size() <= detail::kMaxMessageSize) {
                    try {
                        JsonRpcMessage msg = DeserializeMessage(full_body);
                        DeliverIncoming(std::move(msg));
                        delivered = true;
                    } catch (const std::exception&) {
                    }
                }
                if (!delivered && resp.status_code == 404) {
                    try {
                        auto req_body = JsonValue::Parse(body);
                        if (channel_) {
                            channel_->Send(streamable_http_client_impl::
                                MakeSessionExpiredError(req_body));
                            delivered = true;
                        }
                    } catch (...) {
                    }
                }
                if (!delivered) {
                    MCP_LOG(Error, std::string("HTTP POST returned status ") + std::to_string(resp.status_code));
                    NotifyError("HTTP POST returned status " + std::to_string(resp.status_code));
                }
                return;
            }

            // 202 acknowledges a notification; it never carries a response.
            if (resp.status_code == 202)
            {
                bool had_id = false;
                try {
                    auto jv = JsonValue::Parse(body);
                    had_id = jv.Contains("id");
                } catch (...) {
                }
                if (had_id) {
                    MCP_LOG(Warning, "HTTP POST returned 202 for a request; response lost");
                } else {
                    MCP_LOG(Info, "HTTP POST accepted (202)");
                }
                return;
            }

            auto ct = httpclient_posix_impl::GetHeader(resp, "Content-Type");
            if (ct.find("text/event-stream") != std::string::npos) {
                // Complete blocks were already dispatched from the streaming
                // callback; a held-open stream simply keeps being read.
                return;
            } else {
                if (full_body.empty()) return;
                if (full_body.size() > detail::kMaxMessageSize) {
                    MCP_LOG(Error, "HTTP response exceeded max message size");
                    NotifyError("HTTP response exceeded max message size");
                    return;
                }
                if (is_initialize) {
                    if (auto v = streamable_http_client_impl::NegotiatedVersionFromResponse(
                            full_body)) {
                        StoreNegotiatedVersion(std::move(*v));
                    }
                }
                try {
                    JsonRpcMessage msg = DeserializeMessage(full_body);
                    DeliverIncoming(std::move(msg));
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("HTTP response parse failed: ") + e.what());
                }
                return;
            }
        }
        // Both attempts failed with 401/403.
    }

    void DispatchSseBlock(const std::string& block, bool learn_version) {
        auto parsed = streamable_http_client_impl::ParseSseBlock(block);
        if (parsed.data.empty()) return;
        if (parsed.data.size() > detail::kMaxMessageSize) {
            MCP_LOG(Error, "HTTP SSE block exceeded max message size");
            NotifyError("HTTP SSE block exceeded max message size");
            return;
        }
        if (learn_version && parsed.message) {
            if (auto v = streamable_http_client_impl::NegotiatedVersionFromResponse(
                    parsed.data)) {
                StoreNegotiatedVersion(std::move(*v));
            }
        }
        if (parsed.message) {
            DeliverIncoming(std::move(*parsed.message));
        } else {
            MCP_LOG(Error, "HTTP SSE block parse failed");
        }
    }

    std::string NegotiatedVersion() {
        std::lock_guard<std::mutex> lk(version_mutex_);
        return negotiated_version_;
    }

    std::string CurrentSessionId() {
        std::lock_guard<std::mutex> lk(session_id_mutex_);
        return current_session_id_;
    }

    void StoreSessionId(std::string sid) {
        std::lock_guard<std::mutex> lk(session_id_mutex_);
        current_session_id_ = std::move(sid);
    }

    void StoreNegotiatedVersion(std::string version) {
        std::lock_guard<std::mutex> lk(version_mutex_);
        if (negotiated_version_.empty()) negotiated_version_ = std::move(version);
    }

    HttpClientTransportOptions options_;
    std::mutex session_id_mutex_;
    std::string current_session_id_;
    std::mutex version_mutex_;
    std::string negotiated_version_;
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> delete_pending_{false};
    std::thread listen_thread_;
    std::mutex listen_mutex_;
    std::atomic<streamable_http_client_impl::ListenState> listen_state_{
        streamable_http_client_impl::ListenState::Idle};
    std::mutex last_event_id_mutex_;
    std::string last_event_id_;
    std::atomic<bool> listen_requested_{false};
    // In-flight listen GET's HttpClient, owned by the listen thread; Close()
    // calls Close() on it (never deletes it) to wake a blocked socket read.
    detail::net::HttpClient* listen_client_ = nullptr;

    http_detail::ToolAnnotationCache tool_annotations_;

    bool DeliverIncoming(JsonRpcMessage message) {
        ObserveToolsListResult(message);
        return channel_ && channel_->Send(std::move(message));
    }

    void ObserveToolsListResult(JsonRpcMessage& message) {
        auto* response = std::get_if<JsonRpcResponse>(&message);
        if (!response) return;
        const std::string key = http_detail::KeyFromId(response->id);
        if (!tool_annotations_.ConsumeToolsListRequest(key)) return;
        auto rejected = tool_annotations_.ObserveToolsListResult(response->result);
        for (const auto& name : rejected) {
            MCP_LOG(Warning, "rejecting tool with invalid x-mcp-header: " + name);
        }
    }

    static std::string McpHeaderValue(const JsonValue& value) {
        return http_detail::EncodeHeaderValue(value).value_or(std::string());
    }

    void NoteToolsListRequest(const JsonValue& body) {
        const JsonValue* id = body.Find("id");
        if (!id) return;
        if (id->IsInt()) tool_annotations_.NoteToolsListRequest(std::to_string(id->GetInt()));
        else if (id->IsString()) tool_annotations_.NoteToolsListRequest(id->GetString());
    }

    void AppendParamHeaders(std::unordered_map<std::string, std::string>& headers,
                            const JsonValue& body, const std::string& method) {
        const JsonValue* params = body.Find("params");
        if (!params || !params->IsObject()) return;
        for (const auto& [name, value] : tool_annotations_.BuildParamHeaders(method, *params)) {
            headers[name] = value;
        }
    }
};

} // namespace
#endif

// ═══════════════════════════════════════════════════════════════════════
// Common StreamableHttpClientTransport
// ═══════════════════════════════════════════════════════════════════════

StreamableHttpClientTransport::StreamableHttpClientTransport(
    const HttpClientTransportOptions& options)
    : options_(options) {}

StreamableHttpClientTransport::~StreamableHttpClientTransport() = default;

std::string_view StreamableHttpClientTransport::Name() const {
    return options_.name.empty() ? std::string_view{"streamable-http"} : options_.name;
}

std::shared_ptr<ITransport> StreamableHttpClientTransport::Connect() {
    auto session = std::make_shared<StreamableHttpSessionTransport>(options_);
    session->Start();
    return session;
}

} // namespace mcp
