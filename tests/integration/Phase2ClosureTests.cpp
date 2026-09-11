// Phase2ClosureTests — second-phase feature closure integration tests
//
// Covered features:
//   * Tasks: ToolOptions.execution(mode=Task) + ServerOptions::task_store,
//     CallToolAsTask handle, status notifications, GetTask / CancelTask
//     (cooperative cancellation, terminal states never migrate)
//   * URL elicitation: McpServer::ElicitUrl inside a tool handler,
//     McpClient::SetUrlElicitationHandler, automatic
//     notifications/elicitation/complete, server future resolves as accept
//   * Resilience: McpClient::ListToolsAll aggregation and
//     ClientOptions::max_total_timeout hard cap
//
// Transport matrix:
//   * InMemory        — tasks closure, URL elicitation closure
//   * Streamable HTTP — tasks main path, robustness, URL elicitation closure
//
// Era note: tasks and URL-mode elicitation are gated to non-modern protocol
// versions, so every fixture uses ConnectMode::Legacy whose initialize
// handshake settles on 2025-11-25 on both sides.

#include <mcp/Content.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Methods.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/storage/FileTaskStore.hpp>
#include <mcp/transport/InMemoryTransport.hpp>
#include <mcp/transport/StreamableHttpServerTransport.hpp>
#include <mcp/transport/StreamableHttpClientTransport.hpp>

#include <mcp/test/McpTest.hpp>
#include "../unit/TestServerUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace mcp;
using Ctx = RequestContext<CallToolRequestParams>;

namespace {

template <typename F>
void RunWithTimeout(F&& body) {
    auto future = std::async(std::launch::async, std::forward<F>(body));
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fprintf(stderr,
            "[  FAILED  ] test body hung: call did not complete within 10s\n");
        std::_Exit(1);
    }
    future.get();
}

template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

std::string NotificationField(const JsonRpcNotification& n, const char* key) {
    if (!n.params || !n.params->IsObject()) return "";
    auto* v = n.params->Find(key);
    return (v && v->IsString()) ? v->GetString() : "";
}

ToolOptions MakeTaskToolOptions() {
    ToolExecution exec;
    exec.mode = ToolExecutionMode::Task;
    ToolOptions opts;
    opts.execution = exec;
    return opts;
}

CallToolResult MakeTextResult(std::string text) {
    CallToolResult r;
    r.content.push_back(TextContent{"text", std::move(text)});
    return r;
}

std::filesystem::path MakeTaskStorePath() {
    auto temp_dir = std::filesystem::temp_directory_path();
    return temp_dir / ("mcp_phase2_tasks_" +
        std::string(mcp::test::CurrentTestName()) + ".json");
}

void RemoveQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// Worker that runs for ~500ms in short slices so cancellation is observed
// quickly and the task completes deterministically on its own.
CallToolResult TaskWorkerBody(const Ctx& ctx) {
    for (int i = 0; i < 25; ++i) {
        if (ctx.IsCancellationRequested()) return MakeTextResult("cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return MakeTextResult("task-done");
}

void RegisterUrlGateTool(McpServer& server) {
    server.RegisterTool("url_gate",
        ToolOptions{}.Description("Opens a URL elicitation and reports the action"),
        std::function<CallToolResult(const Ctx&)>(
            [](const Ctx& ctx) -> CallToolResult {
                auto fut = ctx.Server().ElicitUrl("https://example.test/consent",
                    "please approve", std::chrono::seconds(5));
                auto result = fut.get();
                return MakeTextResult("elicit:" + result.action);
            }));
}

} // namespace

// ============================================================
// InMemory: tasks full closure
// ============================================================
struct Phase2InMemoryTaskFixture : mcp::test::TestCase {
    std::filesystem::path store_path;
    std::shared_ptr<FileTaskStore> store;
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::promise<void>> cancellable_started =
        std::make_shared<std::promise<void>>();

    void SetUp() override {
        store_path = MakeTaskStorePath();
        RemoveQuietly(store_path);
        store = std::make_shared<FileTaskStore>(store_path);

        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        sopts.task_store = store;
        server = McpServer::Create(pair.server, sopts);

        server->RegisterTool("task_worker", MakeTaskToolOptions(),
            std::function<CallToolResult(const Ctx&)>(TaskWorkerBody));

        auto started = cancellable_started;
        server->RegisterTool("cancellable_worker", MakeTaskToolOptions(),
            std::function<CallToolResult(const Ctx&)>(
                [started](const Ctx& ctx) -> CallToolResult {
                    started->set_value();
                    for (int i = 0; i < 200; ++i) {
                        if (ctx.IsCancellationRequested())
                            return MakeTextResult("cancelled-observed");
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    return MakeTextResult("ran-to-completion");
                }));

        server_thread = std::thread([this]() { server->Run(); });

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        client = McpClient::Create(pair.client, cops);
    }

    void TearDown() override {
        if (client) client->Close();
        if (server) server->Close();
        if (server_thread.joinable()) server_thread.join();
        store.reset();
        RemoveQuietly(store_path);
    }
};

TEST_F(Phase2InMemoryTaskFixture, TaskFullLifecycle) {
    RunWithTimeout([this]() {
        EXPECT_EQ(std::string(client->GetNegotiatedProtocolVersion()),
                  std::string("2025-11-25"));

        auto statuses_mutex = std::make_shared<std::mutex>();
        auto statuses = std::make_shared<std::vector<std::string>>();
        client->SetNotificationHandler(notifications::kTaskStatus,
            [statuses_mutex, statuses](const JsonRpcNotification& n) {
                std::lock_guard<std::mutex> lock(*statuses_mutex);
                statuses->push_back(NotificationField(n, "status"));
            });

        auto handle = client->CallToolAsTask("task_worker");
        EXPECT_FALSE(handle.task_id.empty());
        EXPECT_EQ(handle.task_id.compare(0, 5, "task-"), 0);
        EXPECT_EQ(handle.status, "working");

        auto done = client->PollTaskToCompletion(
            handle.task_id, std::chrono::milliseconds(100),
            std::chrono::seconds(8));
        EXPECT_EQ(done.status, "completed");
        EXPECT_EQ(done.task_id, handle.task_id);
        ASSERT_TRUE(done.result.has_value());
        auto tool_result = DeserializeCallToolResult(*done.result);
        EXPECT_FALSE(tool_result.is_error);
        ASSERT_GE(tool_result.content.size(), 1u);
        auto* text = std::get_if<TextContent>(&tool_result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "task-done");

        auto final_task = client->GetTask(handle.task_id);
        EXPECT_EQ(final_task.status, "completed");

        auto saw_working = WaitFor([statuses_mutex, statuses]() {
            std::lock_guard<std::mutex> lock(*statuses_mutex);
            return std::find(statuses->begin(), statuses->end(), "working")
                != statuses->end();
        }, std::chrono::seconds(5));
        ASSERT_TRUE(saw_working);
        auto saw_completed = WaitFor([statuses_mutex, statuses]() {
            std::lock_guard<std::mutex> lock(*statuses_mutex);
            return std::find(statuses->begin(), statuses->end(), "completed")
                != statuses->end();
        }, std::chrono::seconds(5));
        ASSERT_TRUE(saw_completed);
    });
}

TEST_F(Phase2InMemoryTaskFixture, CancelRunningTask) {
    RunWithTimeout([this]() {
        auto cancelled_promise = std::make_shared<std::promise<std::string>>();
        auto cancelled_future = cancelled_promise->get_future();
        client->SetNotificationHandler(notifications::kTaskCancelled,
            [cancelled_promise](const JsonRpcNotification& n) {
                try {
                    cancelled_promise->set_value(NotificationField(n, "taskId"));
                } catch (const std::future_error&) {
                }
            });

        auto handle = client->CallToolAsTask("cancellable_worker");
        EXPECT_EQ(handle.status, "working");
        ASSERT_EQ(cancellable_started->get_future().wait_for(
                      std::chrono::seconds(3)),
                  std::future_status::ready);

        client->CancelTask(handle.task_id, "test-cancel");

        auto done = client->PollTaskToCompletion(
            handle.task_id, std::chrono::milliseconds(50),
            std::chrono::seconds(8));
        EXPECT_EQ(done.status, "cancelled");

        auto final_task = client->GetTask(handle.task_id);
        EXPECT_EQ(final_task.status, "cancelled");
        EXPECT_FALSE(final_task.result.has_value());

        ASSERT_EQ(cancelled_future.wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        EXPECT_EQ(cancelled_future.get(), handle.task_id);
    });
}

// ============================================================
// InMemory: URL elicitation closure
// ============================================================
struct Phase2InMemoryElicitFixture : mcp::test::TestCase {
    std::unique_ptr<McpServer> server;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;

    void SetUp() override {
        auto pair = InMemoryTransport::CreatePair();

        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        server = McpServer::Create(pair.server, sopts);
        RegisterUrlGateTool(*server);

        server_thread = std::thread([this]() { server->Run(); });

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        client = McpClient::Create(pair.client, cops);
    }

    void TearDown() override {
        if (client) client->Close();
        if (server) server->Close();
        if (server_thread.joinable()) server_thread.join();
    }
};

TEST_F(Phase2InMemoryElicitFixture, UrlElicitationRoundTrip) {
    RunWithTimeout([this]() {
        auto captured_promise = std::make_shared<std::promise<ElicitRequestParams>>();
        auto captured_future = captured_promise->get_future();
        client->SetUrlElicitationHandler(
            [captured_promise](const ElicitRequestParams& params) {
                captured_promise->set_value(params);
            });

        auto result = client->CallTool("url_gate");
        EXPECT_FALSE(result.is_error);
        ASSERT_GE(result.content.size(), 1u);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "elicit:accept");

        ASSERT_EQ(captured_future.wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        auto params = captured_future.get();
        EXPECT_EQ(params.mode, "url");
        ASSERT_TRUE(params.url.has_value());
        EXPECT_EQ(*params.url, "https://example.test/consent");
        EXPECT_EQ(params.message, "please approve");
        ASSERT_TRUE(params.elicitation_id.has_value());
        EXPECT_FALSE(params.elicitation_id->empty());
    });
}

// ============================================================
// Streamable HTTP: tasks main path
// ============================================================
struct Phase2HttpTaskFixture : mcp::test::TestCase {
    uint16_t port = 0;
    std::filesystem::path store_path;
    std::shared_ptr<FileTaskStore> store;
    std::shared_ptr<StreamableHttpServerTransport> server_transport;
    std::unique_ptr<McpServer> server;
    std::shared_ptr<ITransport> client_transport;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::atomic<bool>> server_initialized =
        std::make_shared<std::atomic<bool>>(false);

    void SetUp() override {
        store_path = MakeTaskStorePath();
        RemoveQuietly(store_path);
        store = std::make_shared<FileTaskStore>(store_path);

        port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 1800));

        StreamableHttpServerOptions topts;
        topts.port = port;
        topts.endpoint = "/mcp";
        server_transport = std::make_shared<StreamableHttpServerTransport>(topts);

        auto initialized_flag = server_initialized;
        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        sopts.task_store = store;
        sopts.on_initialized = [initialized_flag]() {
            initialized_flag->store(true);
        };
        server = McpServer::Create(server_transport, sopts);

        server->RegisterTool("http_task_worker", MakeTaskToolOptions(),
            std::function<CallToolResult(const Ctx&)>(
                [](const Ctx& ctx) -> CallToolResult {
                    for (int i = 0; i < 10; ++i) {
                        if (ctx.IsCancellationRequested())
                            return MakeTextResult("cancelled");
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    return MakeTextResult("http-task-done");
                }));

        server_thread = std::thread([this]() { server->Run(); });
        ASSERT_TRUE(WaitUntilReady(port));

        HttpClientTransportOptions copts;
        copts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        StreamableHttpClientTransport http_client(copts);
        client_transport = http_client.Connect();

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        client = McpClient::Create(client_transport, cops);

        ASSERT_TRUE(WaitFor([initialized_flag]() { return initialized_flag->load(); },
            std::chrono::seconds(5)));
    }

    void TearDown() override {
        if (client) client->Close();
        if (client_transport) client_transport->Close();
        if (server) server->Close();
        if (server_transport) server_transport->Close();
        if (server_thread.joinable()) server_thread.join();
        store.reset();
        RemoveQuietly(store_path);
    }
};

TEST_F(Phase2HttpTaskFixture, TaskLifecycleOverHttp) {
    RunWithTimeout([this]() {
        auto handle = client->CallToolAsTask("http_task_worker");
        EXPECT_FALSE(handle.task_id.empty());
        EXPECT_EQ(handle.status, "working");

        auto done = client->PollTaskToCompletion(
            handle.task_id, std::chrono::milliseconds(100),
            std::chrono::seconds(8));
        EXPECT_EQ(done.status, "completed");
        ASSERT_TRUE(done.result.has_value());
        auto tool_result = DeserializeCallToolResult(*done.result);
        EXPECT_FALSE(tool_result.is_error);
        ASSERT_GE(tool_result.content.size(), 1u);
        auto* text = std::get_if<TextContent>(&tool_result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "http-task-done");

        auto final_task = client->GetTask(handle.task_id);
        EXPECT_EQ(final_task.status, "completed");
    });
}

// ============================================================
// Streamable HTTP: robustness (ListToolsAll + max_total_timeout)
// ============================================================
struct Phase2HttpResilienceFixture : mcp::test::TestCase {
    uint16_t port = 0;
    std::shared_ptr<StreamableHttpServerTransport> server_transport;
    std::unique_ptr<McpServer> server;
    std::shared_ptr<ITransport> client_transport;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::atomic<bool>> slow_released =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> server_initialized =
        std::make_shared<std::atomic<bool>>(false);

    void SetUp() override {
        port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 1900));

        StreamableHttpServerOptions topts;
        topts.port = port;
        topts.endpoint = "/mcp";
        server_transport = std::make_shared<StreamableHttpServerTransport>(topts);

        auto initialized_flag = server_initialized;
        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        sopts.on_initialized = [initialized_flag]() {
            initialized_flag->store(true);
        };
        server = McpServer::Create(server_transport, sopts);

        for (const char* name : {"alpha", "beta", "gamma"}) {
            server->RegisterTool(name,
                ToolOptions{}.Description("Listed tool"),
                std::function<CallToolResult(const Ctx&)>(
                    [](const Ctx&) { return MakeTextResult("ok"); }));
        }

        auto released = slow_released;
        server->RegisterTool("slow_tool",
            ToolOptions{}.Description("Runs past the client total budget"),
            std::function<CallToolResult(const Ctx&)>(
                [released](const Ctx&) -> CallToolResult {
                    for (int i = 0; i < 40 && !released->load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return MakeTextResult("slow-done");
                }));

        server_thread = std::thread([this]() { server->Run(); });
        ASSERT_TRUE(WaitUntilReady(port));

        HttpClientTransportOptions copts;
        copts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        StreamableHttpClientTransport http_client(copts);
        client_transport = http_client.Connect();

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        cops.max_total_timeout = std::chrono::seconds(1);
        client = McpClient::Create(client_transport, cops);

        ASSERT_TRUE(WaitFor([initialized_flag]() { return initialized_flag->load(); },
            std::chrono::seconds(5)));
    }

    void TearDown() override {
        slow_released->store(true);
        if (client) client->Close();
        if (client_transport) client_transport->Close();
        if (server) server->Close();
        if (server_transport) server_transport->Close();
        if (server_thread.joinable()) server_thread.join();
    }
};

TEST_F(Phase2HttpResilienceFixture, ListToolsAllAggregates) {
    RunWithTimeout([this]() {
        auto all = client->ListToolsAll();
        EXPECT_FALSE(all.next_cursor.has_value());

        auto single = client->ListTools();
        EXPECT_EQ(all.tools.size(), single.tools.size());

        bool found_alpha = false;
        bool found_beta = false;
        bool found_gamma = false;
        for (const auto& t : all.tools) {
            if (t.name == "alpha") found_alpha = true;
            if (t.name == "beta") found_beta = true;
            if (t.name == "gamma") found_gamma = true;
        }
        EXPECT_TRUE(found_alpha);
        EXPECT_TRUE(found_beta);
        EXPECT_TRUE(found_gamma);
    });
}

TEST_F(Phase2HttpResilienceFixture, MaxTotalTimeoutTruncatesRequest) {
    RunWithTimeout([this]() {
        auto start = std::chrono::steady_clock::now();
        auto call = std::async(std::launch::async, [this]() -> std::exception_ptr {
            try {
                client->CallTool("slow_tool");
                return nullptr;
            } catch (...) {
                return std::current_exception();
            }
        });

        ASSERT_EQ(call.wait_for(std::chrono::seconds(8)),
                  std::future_status::ready);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        auto exc = call.get();
        ASSERT_TRUE(exc != nullptr);
        try {
            std::rethrow_exception(exc);
        } catch (const McpError& e) {
            EXPECT_EQ(e.Code(), McpErrorCode::RequestTimeout);
        }
        EXPECT_GE(elapsed.count(), static_cast<int64_t>(900));
        EXPECT_LT(elapsed.count(), static_cast<int64_t>(9500));

        slow_released->store(true);
    });
}

// ============================================================
// Streamable HTTP: URL elicitation closure
// ============================================================
struct Phase2HttpElicitFixture : mcp::test::TestCase {
    uint16_t port = 0;
    std::shared_ptr<StreamableHttpServerTransport> server_transport;
    std::unique_ptr<McpServer> server;
    std::shared_ptr<ITransport> client_transport;
    std::unique_ptr<McpClient> client;
    std::thread server_thread;
    std::shared_ptr<std::atomic<bool>> server_initialized =
        std::make_shared<std::atomic<bool>>(false);

    void SetUp() override {
        port = PickFreePort(static_cast<uint16_t>(kTestBasePort + 2000));

        StreamableHttpServerOptions topts;
        topts.port = port;
        topts.endpoint = "/mcp";
        server_transport = std::make_shared<StreamableHttpServerTransport>(topts);

        auto initialized_flag = server_initialized;
        ServerOptions sopts;
        sopts.server_info = Implementation{"TestServer", "1.0.0"};
        sopts.on_initialized = [initialized_flag]() {
            initialized_flag->store(true);
        };
        server = McpServer::Create(server_transport, sopts);
        RegisterUrlGateTool(*server);

        server_thread = std::thread([this]() { server->Run(); });
        ASSERT_TRUE(WaitUntilReady(port));

        HttpClientTransportOptions copts;
        copts.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
        StreamableHttpClientTransport http_client(copts);
        client_transport = http_client.Connect();

        ClientOptions cops;
        cops.client_info = Implementation{"TestClient", "1.0.0"};
        cops.connect_mode = ConnectMode::Legacy;
        client = McpClient::Create(client_transport, cops);

        ASSERT_TRUE(WaitFor([initialized_flag]() { return initialized_flag->load(); },
            std::chrono::seconds(5)));
    }

    void TearDown() override {
        if (client) client->Close();
        if (client_transport) client_transport->Close();
        if (server) server->Close();
        if (server_transport) server_transport->Close();
        if (server_thread.joinable()) server_thread.join();
    }
};

TEST_F(Phase2HttpElicitFixture, UrlElicitationOverHttp) {
    RunWithTimeout([this]() {
        auto captured_promise = std::make_shared<std::promise<ElicitRequestParams>>();
        auto captured_future = captured_promise->get_future();
        client->SetUrlElicitationHandler(
            [captured_promise](const ElicitRequestParams& params) {
                captured_promise->set_value(params);
            });

        auto result = client->CallTool("url_gate");
        EXPECT_FALSE(result.is_error);
        ASSERT_GE(result.content.size(), 1u);
        auto* text = std::get_if<TextContent>(&result.content[0]);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->text, "elicit:accept");

        ASSERT_EQ(captured_future.wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        auto params = captured_future.get();
        EXPECT_EQ(params.mode, "url");
        ASSERT_TRUE(params.url.has_value());
        EXPECT_EQ(*params.url, "https://example.test/consent");
        EXPECT_EQ(params.message, "please approve");
    });
}
