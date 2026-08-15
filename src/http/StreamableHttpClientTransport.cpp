// StreamableHttpClientTransport.cpp - Streamable HTTP client transport (Win32 WinHTTP / POSIX self-hosted)

#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/Log.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/transport/detail/Url.hpp>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
// Windows.h defines GetObject macro which conflicts with JsonValue::GetObject
#pragma push_macro("GetObject")
#undef GetObject
#endif

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

#ifndef _WIN32
#include <transport/detail/net/HttpClient.hpp>
#endif

namespace mcp {

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
        auto j = SerializeMessage(std::move(message));
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            send_queue_.push(j);
        }
        send_cv_.notify_one();
    }

private:
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
        if (current_session_id_.empty()) return;
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
            ToWideStr(current_session_id_) + L"\r\n";
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
            // Add MCP headers
            hdrs += L"MCP-Protocol-Version: 2026-07-28\r\n";
            if (!current_session_id_.empty()) {
                hdrs += L"Mcp-Session-Id: " +
                    ToWideStr(current_session_id_) + L"\r\n";
            }
            try {
                auto body_jv2 = JsonValue::Parse(body);
                if (auto* m = body_jv2.Find("method"); m && m->IsString()) {
                    hdrs += L"Mcp-Method: " + ToWideStr(m->GetString()) + L"\r\n";
                }
                if (auto* p = body_jv2.Find("params"); p && p->IsObject()) {
                    // iterate params manually to avoid Windows GetObject macro expansion
                    const auto& obj = p->GetObject();
                    for (const auto& [k, v] : obj) {
                        if (v.IsString()) {
                            hdrs += L"Mcp-Param-" + ToWideStr(k) + L": " + ToWideStr(v.GetString()) + L"\r\n";
                        } else if (v.IsInt()) {
                            hdrs += L"Mcp-Param-" + ToWideStr(k) + L": " + ToWideStr(std::to_string(v.GetInt())) + L"\r\n";
                        } else if (v.IsBool()) {
                            hdrs += L"Mcp-Param-" + ToWideStr(k) + L": " + ToWideStr(v.GetBool() ? "true" : "false") + L"\r\n";
                        } else if (v.IsDouble()) {
                            hdrs += L"Mcp-Param-" + ToWideStr(k) + L": " + ToWideStr(std::to_string(v.GetDouble())) + L"\r\n";
                        }
                    }
                    if (auto n = obj.find("name"); n != obj.end() && n->second.IsString()) {
                        hdrs += L"Mcp-Name: " + ToWideStr(n->second.GetString()) + L"\r\n";
                    } else if (auto u = obj.find("uri"); u != obj.end() && u->second.IsString()) {
                        hdrs += L"Mcp-Name: " + ToWideStr(u->second.GetString()) + L"\r\n";
                    }
                }
            } catch (...) {
                MCP_LOG(Warning, "HTTP header parse failed");
                hdrs += L"Mcp-Method: tools/call\r\n";
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
                current_session_id_ = wide_to_utf8(sid_buf);
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
                bool delivered = false;
                if (!err_body.empty() && err_body.size() <= detail::kMaxMessageSize) {
                    try {
                        JsonRpcMessage msg = DeserializeMessage(err_body);
                        if (channel_) channel_->Send(std::move(msg));
                        delivered = true;
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
                // POST SSE response stream: read until the server closes it
                // after the final event, then dispatch every block.
                sse_request_ = hRequest;
                std::string sse_body;
                char sbuf[4096];
                DWORD sread = 0;
                while (WinHttpReadData(hRequest, sbuf, sizeof(sbuf), &sread) && sread > 0) {
                    sse_body.append(sbuf, sread);
                    sread = 0;
                }
                sse_request_ = nullptr;
                size_t pos;
                while ((pos = sse_body.find("\n\n")) != std::string::npos) {
                    std::string block = sse_body.substr(0, pos);
                    sse_body.erase(0, pos + 2);
                    DispatchSseBlock(block);
                }
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
                    try {
                        JsonRpcMessage msg = DeserializeMessage(resp_body);
                        if (channel_) channel_->Send(std::move(msg));
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

    void DispatchSseBlock(const std::string& block) {
        // Parse SSE: event: message\ndata: {...}
        std::string data;
        std::istringstream ss(block);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") == 0) {
                auto val = line.substr(5);
                auto n = val.find_first_not_of(" \t");
                if (n != std::string::npos) val = val.substr(n);
                if (!data.empty()) data += "\n";
                data += val;
            }
        }
        if (!data.empty()) {
            if (data.size() > detail::kMaxMessageSize) {
                MCP_LOG(Error, "HTTP SSE block exceeded max message size");
                NotifyError("HTTP SSE block exceeded max message size");
                return;
            }
            try {
                JsonRpcMessage msg = DeserializeMessage(data);
                if (channel_) channel_->Send(std::move(msg));
            } catch (...) { MCP_LOG(Error, "HTTP SSE block parse failed"); }
        }
    }

    HttpClientTransportOptions options_;
    std::string current_session_id_;
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> delete_pending_{false};
    // Handle of the request whose SSE response is being read by the send
    // thread; Close() shortens its receive timeout to interrupt the read.
    std::atomic<HINTERNET> sse_request_{nullptr};
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
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = resp.headers.find(lower);
    return it == resp.headers.end() ? std::string{} : it->second;
}

} // namespace httpclient_posix_impl

namespace {

constexpr int kHttpRequestTimeoutSeconds = 30;

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
        detail::JoinThreadSafely(send_thread_);
        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;
        auto body = SerializeMessage(std::move(message));
        { std::lock_guard<std::mutex> lk(send_mutex_); send_queue_.push(std::move(body)); }
        send_cv_.notify_one();
    }

private:
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
        if (current_session_id_.empty()) return;
        detail::net::HttpRequestSpec req;
        req.method = "DELETE";
        req.url = options_.endpoint;
        req.timeout = std::chrono::milliseconds(kHttpRequestTimeoutSeconds * 1000);
        req.headers["Mcp-Session-Id"] = current_session_id_;
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
            headers["MCP-Protocol-Version"] = "2026-07-28";
            if (!current_session_id_.empty()) {
                headers["Mcp-Session-Id"] = current_session_id_;
            }
            try {
                auto jv = JsonValue::Parse(body);
                if (auto* m = jv.Find("method"); m && m->IsString())
                    headers["Mcp-Method"] = m->GetString();
                if (auto* p = jv.Find("params"); p && p->IsObject()) {
                    const auto& obj = p->GetObject();
                    for (const auto& [k, v] : obj) {
                        if (v.IsString()) {
                            headers["Mcp-Param-" + k] = v.GetString();
                        } else if (v.IsInt()) {
                            headers["Mcp-Param-" + k] = std::to_string(v.GetInt());
                        } else if (v.IsBool()) {
                            headers["Mcp-Param-" + k] = v.GetBool() ? "true" : "false";
                        } else if (v.IsDouble()) {
                            headers["Mcp-Param-" + k] = std::to_string(v.GetDouble());
                        }
                    }
                    if (auto n = obj.find("name"); n != obj.end() && n->second.IsString())
                        headers["Mcp-Name"] = n->second.GetString();
                    else if (auto u = obj.find("uri"); u != obj.end() && u->second.IsString())
                        headers["Mcp-Name"] = u->second.GetString();
                }
            } catch (...) {
                headers["Mcp-Method"] = "unknown";
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
            try {
                resp = client.Request(req);
            } catch (...) {
                MCP_LOG(Error, "HTTP POST failed");
                NotifyError("HTTP POST failed");
                return;
            }

            // Capture a session id from any response; later requests carry it.
            auto sid = httpclient_posix_impl::GetHeader(resp, "Mcp-Session-Id");
            if (!sid.empty()) {
                current_session_id_ = std::move(sid);
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
                bool delivered = false;
                if (!resp.body.empty() && resp.body.size() <= detail::kMaxMessageSize) {
                    try {
                        JsonRpcMessage msg = DeserializeMessage(resp.body);
                        if (channel_) channel_->Send(std::move(msg));
                        delivered = true;
                    } catch (const std::exception&) {
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
                auto sse_data = resp.body;
                size_t pos;
                while ((pos = sse_data.find("\n\n")) != std::string::npos) {
                    std::string block = sse_data.substr(0, pos);
                    sse_data.erase(0, pos + 2);
                    DispatchSseBlock(block);
                }
                return;
            } else {
                if (resp.body.empty()) return;
                if (resp.body.size() > detail::kMaxMessageSize) {
                    MCP_LOG(Error, "HTTP response exceeded max message size");
                    NotifyError("HTTP response exceeded max message size");
                    return;
                }
                try {
                    JsonRpcMessage msg = DeserializeMessage(resp.body);
                    if (channel_) channel_->Send(std::move(msg));
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("HTTP response parse failed: ") + e.what());
                }
                return;
            }
        }
        // Both attempts failed with 401/403.
    }

    void DispatchSseBlock(const std::string& block) {
        std::string data;
        std::istringstream ss(block);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") == 0) {
                auto val = line.substr(5);
                auto n = val.find_first_not_of(" \t");
                if (n != std::string::npos) val = val.substr(n);
                if (!data.empty()) data += "\n";
                data += val;
            }
        }
        if (!data.empty()) {
            if (data.size() > detail::kMaxMessageSize) {
                MCP_LOG(Error, "HTTP SSE block exceeded max message size");
                NotifyError("HTTP SSE block exceeded max message size");
                return;
            }
            try {
                JsonRpcMessage msg = DeserializeMessage(data);
                if (channel_) channel_->Send(std::move(msg));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("SSE parse failed: ") + e.what());
            }
        }
    }

    HttpClientTransportOptions options_;
    std::string current_session_id_;
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> delete_pending_{false};
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
