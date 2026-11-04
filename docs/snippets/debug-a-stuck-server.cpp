#include <mcp/Implementation.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/SpanHooks.hpp>
#include <mcp/Transport.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/client/RequestHandle.hpp>
#include <mcp/protocol/IncomingRequestMeta.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace mcp;

namespace debug_a_stuck_server {

// region enable-internal-logs
void RaiseVerbosity() {
    SetLogLevel(LogLevel::Debug);
    const int configured = static_cast<int>(GetLogLevel());
    (void)configured;
}

LogLevel LevelFromEnvironment() {
    return GetLogLevel();
}
// endregion

// region custom-log-sink
void InstallLogSink(std::mutex& guard, std::vector<std::string>& lines) {
    SetLogHandler([&guard, &lines](const LogRecord& record) {
        std::lock_guard<std::mutex> lock(guard);
        lines.push_back(std::to_string(static_cast<int>(record.level)) + " [" +
            std::string(record.tag) + "] " + std::string(record.message) +
            " @" + std::to_string(record.line));
    });
}

void RemoveLogSink() {
    SetLogHandler(nullptr);
}
// endregion

// region span-dump
struct SpanCollect {
    std::mutex mutex;
    std::vector<std::string> lines;

    void Record(const SpanEvent& event) {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(event.name + " " + event.method + " id=" + event.request_id +
            (event.failed ? " failed" : ""));
    }
};

ClientOptions ClientWithSpans(SpanCollect& collector) {
    ClientOptions options;
    options.client_info = Implementation{"debug-probe", "0.1.0"};
    options.span_handler = [&collector](const SpanEvent& event) {
        collector.Record(event);
    };
    return options;
}

ServerOptions ServerWithSpans(SpanCollect& collector) {
    ServerOptions options;
    options.span_handler = [&collector](const SpanEvent& event) {
        collector.Record(event);
    };
    return options;
}
// endregion

// region held-promise
class HeldPromises {
public:
    void Take(std::promise<JsonValue> promise) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(promise));
        ++count_;
    }

    void ReleaseOne() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return;
        JsonValue result(JsonValue::object_tag);
        result["released"] = JsonValue(true);
        items_.front().set_value(std::move(result));
        items_.pop_front();
    }

    int64_t Held() const {
        return count_.load();
    }

private:
    std::mutex mutex_;
    std::deque<std::promise<JsonValue>> items_;
    std::atomic<int64_t> count_{0};
};

std::shared_ptr<McpSessionHandler> MakeStalledServer(
    std::shared_ptr<ITransport> session, HeldPromises& held) {
    auto server = std::make_shared<McpSessionHandler>(
        std::move(session), MakeWireCodec(kLatestProtocolVersion));
    server->SetRequestHandler("demo/slow",
        [&held](const JsonRpcRequest&, std::promise<JsonValue> promise) {
            held.Take(std::move(promise));
        });
    server->Start();
    return server;
}
// endregion

// region client-probe
struct ProbeReport {
    bool answered;
    std::size_t progress_frames;
};

ProbeReport ProbeWithTimeout(McpClient& client) {
    std::atomic<std::size_t> frames{0};

    RequestOptions options;
    options.timeout = std::chrono::milliseconds(1500);
    options.on_progress = [&frames](const ProgressNotificationParams&) {
        ++frames;
    };

    JsonValue params(JsonValue::object_tag);
    params["name"] = JsonValue("demo");

    RequestHandle handle = client.SendRequest("tools/call", std::move(params), options);
    const RequestOutcome outcome = handle.future.get();
    return ProbeReport{std::holds_alternative<JsonValue>(outcome), frames.load()};
}
// endregion

// region full
std::string RunDiagnosticProbe() {
    auto pair = InMemoryTransport::CreatePair();
    HeldPromises held;
    auto stalled = MakeStalledServer(pair.server, held);

    ClientOptions options;
    options.client_info = Implementation{"debug-probe", "0.1.0"};
    auto client = McpClient::Create(pair.client, options);

    JsonValue params(JsonValue::object_tag);
    params["name"] = JsonValue("demo");

    RequestOptions call_options;
    call_options.timeout = std::chrono::milliseconds(300);
    RequestHandle handle = client->SendRequest("tools/call", std::move(params), call_options);
    handle.Cancel("probe done");

    const std::string line = std::to_string(held.Held());
    std::fprintf(stderr, "held promises: %s\n", line.c_str());

    stalled->Close();
    return line;
}
// endregion

} // namespace debug_a_stuck_server
