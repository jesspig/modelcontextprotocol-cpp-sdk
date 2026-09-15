#pragma once
// McpTimeout.hpp — 用例体超时护栏（超时记为失败并放弃当前用例）

#include "McpApi.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace mcp::test {

template <typename F>
void RunWithTimeout(const char* file, int line, std::chrono::milliseconds timeout, F&& body) {
    std::atomic<bool> done{false};
    std::exception_ptr error;
    std::thread worker([&] {
        try { body(); }
        catch (...) { error = std::current_exception(); }
        done.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            AssertionFailure(file, line,
                "test body hung: did not complete within " +
                    std::to_string(timeout.count()) + " ms");
            if (TestCase* t = CurrentTest()) t->MarkAbandoned();
            worker.detach();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    if (error) std::rethrow_exception(error);
}

template <typename F>
void RunWithTimeout(const char* file, int line, F&& body) {
    RunWithTimeout(file, line, std::chrono::seconds(10), std::forward<F>(body));
}

}  // namespace mcp::test

#define MCP_RUN_WITH_TIMEOUT(...) \
    ::mcp::test::RunWithTimeout(__FILE__, __LINE__, __VA_ARGS__)
