// McpTestRunner.cpp — 测试注册、过滤、断言失败记录与运行入口

#include <mcp/test/McpTest.hpp>
#include <mcp/test/McpAssert.hpp>
#include <mcp/test/McpTestInfo.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mcp::test {

namespace {

std::atomic<TestCase*> g_current_test{nullptr};
std::atomic<const TestEntry*> g_current_entry{nullptr};

bool GlobMatch(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0;
    size_t star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool MatchEntry(const TestEntry& e, std::string_view pattern) {
    return GlobMatch(pattern, e.suite + "." + e.name);
}

}  // namespace

void TestCase::RecordFailure(std::string message) {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    failures_.push_back(std::move(message));
    ++failure_count_;
}

void TestCase::ClearFailures() {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    failures_.clear();
    failure_count_ = 0;
}

Registry& Registry::Instance() {
    static Registry instance;
    return instance;
}

void Registry::Register(std::string suite, std::string name, TestFactory factory) {
    entries_.push_back(TestEntry{std::move(suite), std::move(name), std::move(factory)});
}

const std::vector<TestEntry>& Registry::Entries() const {
    return entries_;
}

std::vector<const TestEntry*> Registry::Select(std::string_view filter) const {
    std::vector<const TestEntry*> result;
    if (filter.empty()) {
        for (const auto& e : entries_) result.push_back(&e);
        return result;
    }
    std::string_view positive = filter;
    std::string_view negative;
    size_t dash = filter.find('-');
    if (dash != std::string_view::npos) {
        positive = filter.substr(0, dash);
        negative = filter.substr(dash + 1);
    }
    for (const auto& e : entries_) {
        if (!positive.empty() && !MatchEntry(e, positive)) continue;
        if (!negative.empty() && MatchEntry(e, negative)) continue;
        result.push_back(&e);
    }
    return result;
}

void SetCurrentTest(TestCase* test) {
    g_current_test.store(test);
}

TestCase* CurrentTest() {
    return g_current_test.load();
}

std::string_view CurrentTestName() {
    const TestEntry* e = g_current_entry.load();
    return e ? e->name : std::string_view();
}

std::string_view CurrentSuiteName() {
    const TestEntry* e = g_current_entry.load();
    return e ? e->suite : std::string_view();
}

void AssertionFailure(const char* file, int line,
                      std::string_view expr_a, std::string_view expr_b,
                      std::string_view val_a, std::string_view val_b) {
    std::string msg = std::string(file) + ":" + std::to_string(line) +
        ": Failure: Expected " + std::string(expr_a) + " == " + std::string(expr_b) +
        ", got [" + std::string(val_a) + "] vs [" + std::string(val_b) + "]";
    if (TestCase* t = g_current_test.load()) {
        t->RecordFailure(std::move(msg));
    } else {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

void AssertionFailure(const char* file, int line, std::string_view message) {
    std::string msg = std::string(file) + ":" + std::to_string(line) +
        ": Failure: " + std::string(message);
    if (TestCase* t = g_current_test.load()) {
        t->RecordFailure(std::move(msg));
    } else {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int RunAll(int argc, char** argv) {
    std::string filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--gtest_filter=", 0) == 0) {
            filter = a.substr(15);
        } else if (a == "--list-tests") {
            list = true;
        } else if (a == "--help") {
            std::printf("Usage: %s [--gtest_filter=Suite.Case] [--list-tests]\n", argv[0]);
            return 0;
        }
    }

    auto selected = Registry::Instance().Select(filter);
    if (list) {
        for (const auto* e : selected) {
            std::printf("%s.%s\n", e->suite.c_str(), e->name.c_str());
        }
        return 0;
    }

    int failed = 0;
    int ran = 0;
    std::vector<const TestEntry*> failed_entries;
    auto start_all = std::chrono::steady_clock::now();
    for (const auto* e : selected) {
        if (e->name.rfind("DISABLED_", 0) == 0) {
            std::printf("[  SKIPPED ] %s.%s\n", e->suite.c_str(), e->name.c_str());
            continue;
        }
        std::printf("[ RUN      ] %s.%s\n", e->suite.c_str(), e->name.c_str());
        auto test = e->factory();
        g_current_entry.store(e);
        SetCurrentTest(test.get());
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        try {
            test->SetUp();
        } catch (const std::exception& ex) {
            test->RecordFailure("SetUp threw: " + std::string(ex.what()));
            ok = false;
        } catch (...) {
            test->RecordFailure("SetUp threw unknown exception");
            ok = false;
        }
        if (ok) {
            try {
                test->RunBody();
            } catch (const std::exception& ex) {
                test->RecordFailure("test body threw: " + std::string(ex.what()));
            } catch (...) {
                test->RecordFailure("test body threw unknown exception");
            }
        }
        try {
            test->TearDown();
        } catch (...) {
            test->RecordFailure("TearDown threw");
        }
        SetCurrentTest(nullptr);
        g_current_entry.store(nullptr);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        ++ran;
        if (test->HasFailures()) {
            ++failed;
            failed_entries.push_back(e);
            std::printf("[  FAILED  ] %s.%s (%lld ms)\n", e->suite.c_str(), e->name.c_str(),
                        static_cast<long long>(ms));
            for (const auto& f : test->Failures()) {
                std::printf("  %s\n", f.c_str());
            }
        } else {
            std::printf("[       OK ] %s.%s (%lld ms)\n", e->suite.c_str(), e->name.c_str(),
                        static_cast<long long>(ms));
        }
    }
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_all).count();
    std::printf("[==========] %d tests ran. (%lld ms total)\n", ran,
                static_cast<long long>(total_ms));
    if (failed) {
        std::printf("[  FAILED  ] %d tests, listed below:\n", failed);
        for (const auto* e : failed_entries) {
            std::printf("[  FAILED  ] %s.%s\n", e->suite.c_str(), e->name.c_str());
        }
        return 1;
    }
    std::printf("[  PASSED  ] %d tests.\n", ran);
    return 0;
}

}  // namespace mcp::test
