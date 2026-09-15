// McpTestRunner.cpp — 测试注册、过滤、断言失败记录与运行入口

#include <mcp/test/McpTest.hpp>
#include <mcp/test/McpAssert.hpp>
#include <mcp/test/McpTestInfo.hpp>
#include <mcp/test/McpTrace.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::test {

namespace {

std::atomic<TestCase*> g_current_test{nullptr};
std::atomic<const TestEntry*> g_current_entry{nullptr};
thread_local TestCase* t_current_test = nullptr;

// ── 全局环境与监听器存储 ──

std::vector<std::unique_ptr<Environment>>& GlobalEnvironments() {
    static std::vector<std::unique_ptr<Environment>> environments;
    return environments;
}

std::vector<TestEndListener>& TestEndListeners() {
    static std::vector<TestEndListener> listeners;
    return listeners;
}

// ── 过滤匹配 ──

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

std::vector<std::string_view> SplitPatterns(std::string_view text) {
    std::vector<std::string_view> patterns;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t colon = text.find(':', start);
        const size_t end = colon == std::string_view::npos ? text.size() : colon;
        if (end > start) patterns.push_back(text.substr(start, end - start));
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    return patterns;
}

bool MatchAny(const TestEntry& e, const std::vector<std::string_view>& patterns) {
    for (const std::string_view pattern : patterns) {
        if (MatchEntry(e, pattern)) return true;
    }
    return false;
}

// ── 运行期状态 ──

struct RecordedTest {
    std::string suite;
    std::string name;
    std::string status;
    long long elapsed_ms = 0;
    std::map<std::string, std::string> properties;
    std::vector<std::string> failures;
};

struct RecordedSuite {
    std::string name;
    std::vector<RecordedTest> tests;
};

struct JsonReport {
    std::vector<RecordedSuite> suites;
    std::map<std::string, std::size_t> suite_index;
    long long tests = 0;
    long long failures = 0;
    long long skipped = 0;

    void Append(RecordedTest record) {
        auto it = suite_index.find(record.suite);
        if (it == suite_index.end()) {
            it = suite_index.emplace(record.suite, suites.size()).first;
            suites.push_back(RecordedSuite{record.suite, {}});
        }
        if (record.status == "FAILED") ++failures;
        if (record.status == "SKIPPED") ++skipped;
        ++tests;
        suites[it->second].tests.push_back(std::move(record));
    }
};

struct SuiteState {
    int remaining = 0;
    LifecycleHook setup;
    LifecycleHook teardown;
    bool setup_called = false;
};

struct EntryOutcome {
    bool failed = false;
    bool skipped = false;
};

struct Options {
    std::string filter;
    std::string json_output_path;
    long long repeat = 1;
    unsigned long long random_seed = 0;
    bool list_tests = false;
    bool gtest_list_tests = false;
    bool shuffle = false;
    bool break_on_failure = false;
    bool help = false;
};

struct ParsedCommandLine {
    Options options;
    bool exit_now = false;
    int exit_code = 0;
};

bool IsDisabled(const TestEntry* entry) {
    return entry->suite.rfind("DISABLED_", 0) == 0 || entry->name.rfind("DISABLED_", 0) == 0;
}

bool ParseLongLong(std::string_view text, long long& value) {
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        const long long parsed = std::stoll(std::string(text), &consumed);
        if (consumed != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseUnsignedLongLong(std::string_view text, unsigned long long& value) {
    if (text.empty() || text.front() == '-') return false;
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(std::string(text), &consumed);
        if (consumed != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(c));
                    out += buffer;
                } else {
                    out += raw;
                }
                break;
        }
    }
    return out;
}

void AppendTraceChain(std::string& message) {
    const std::string chain = CurrentTraceChain();
    if (!chain.empty()) message += chain;
}

bool WriteJsonReport(const std::string& path, const JsonReport& report) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n";
    out << "  \"tests\": " << report.tests << ",\n";
    out << "  \"failures\": " << report.failures << ",\n";
    out << "  \"skipped\": " << report.skipped << ",\n";
    out << "  \"testsuites\": [";
    for (std::size_t i = 0; i < report.suites.size(); ++i) {
        const RecordedSuite& suite = report.suites[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    {\"name\": \"" << JsonEscape(suite.name) << "\", \"tests\": [";
        for (std::size_t j = 0; j < suite.tests.size(); ++j) {
            const RecordedTest& test = suite.tests[j];
            out << (j == 0 ? "\n" : ",\n");
            out << "      {\"name\": \"" << JsonEscape(test.name)
                << "\", \"status\": \"" << test.status
                << "\", \"elapsed_ms\": " << test.elapsed_ms
                << ", \"properties\": {";
            bool first_property = true;
            for (const auto& property : test.properties) {
                out << (first_property ? "" : ", ");
                first_property = false;
                out << "\"" << JsonEscape(property.first) << "\": \""
                    << JsonEscape(property.second) << "\"";
            }
            out << "}";
            if (!test.failures.empty()) {
                out << ", \"failures\": [";
                bool first_failure = true;
                for (const auto& failure : test.failures) {
                    out << (first_failure ? "" : ", ");
                    first_failure = false;
                    out << "\"" << JsonEscape(failure) << "\"";
                }
                out << "]";
            }
            out << "}";
        }
        out << "\n    ]}";
    }
    out << "\n  ]\n}\n";
    out.flush();
    return static_cast<bool>(out);
}

// ── 命令行解析 ──

void PrintHelp(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "Options:\n"
        "  --gtest_filter=<f>          Filter: POSITIVE[:POSITIVE...][-NEGATIVE[:NEGATIVE...]]\n"
        "  --list-tests                List selected tests as Suite.Case lines\n"
        "  --gtest_list_tests          List selected tests in gtest style\n"
        "  --gtest_repeat=<n>          Run all tests n times (n >= 1)\n"
        "  --gtest_shuffle             Shuffle order, keeping each suite contiguous\n"
        "  --gtest_random_seed=<n>     Shuffle seed (0 = generate from clock)\n"
        "  --gtest_break_on_failure    Abort after printing the first failing test\n"
        "  --gtest_output=json:<path>  Write a JSON report covering all rounds\n"
        "  --help                      Show this help\n",
        program);
}

ParsedCommandLine ParseCommandLine(int argc, char** argv) {
    ParsedCommandLine parsed;
    Options& opts = parsed.options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--gtest_filter=", 0) == 0) {
            opts.filter = arg.substr(15);
        } else if (arg == "--list-tests") {
            opts.list_tests = true;
        } else if (arg == "--gtest_list_tests") {
            opts.gtest_list_tests = true;
        } else if (arg.rfind("--gtest_repeat=", 0) == 0) {
            long long repeat = 0;
            if (!ParseLongLong(std::string_view(arg).substr(15), repeat)) {
                std::fprintf(stderr, "Invalid --gtest_repeat value: %s\n", arg.c_str());
                parsed.exit_now = true;
                parsed.exit_code = 1;
                return parsed;
            }
            if (repeat < 1) {
                std::fprintf(stderr, "Error: --gtest_repeat requires a value >= 1, got %s\n",
                             arg.c_str());
                parsed.exit_now = true;
                parsed.exit_code = 1;
                return parsed;
            }
            opts.repeat = repeat;
        } else if (arg.rfind("--gtest_random_seed=", 0) == 0) {
            unsigned long long seed = 0;
            if (!ParseUnsignedLongLong(std::string_view(arg).substr(20), seed)) {
                std::fprintf(stderr, "Invalid --gtest_random_seed value: %s\n", arg.c_str());
                parsed.exit_now = true;
                parsed.exit_code = 1;
                return parsed;
            }
            opts.random_seed = seed;
        } else if (arg == "--gtest_shuffle") {
            opts.shuffle = true;
        } else if (arg == "--gtest_break_on_failure") {
            opts.break_on_failure = true;
        } else if (arg.rfind("--gtest_output=json:", 0) == 0) {
            opts.json_output_path = arg.substr(20);
        } else if (arg == "--help") {
            opts.help = true;
        } else {
            std::fprintf(stderr, "Warning: unknown argument ignored: %s\n", arg.c_str());
        }
    }
    return parsed;
}

// ── 执行顺序与套件状态 ──

std::vector<const TestEntry*> BuildExecutionOrder(
    const std::vector<const TestEntry*>& selected, const Options& options) {
    if (!options.shuffle) return selected;
    unsigned long long seed = options.random_seed;
    if (seed == 0) {
        seed = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::printf("[ RANDOM SEED ] %llu\n", seed);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed & 0xFFFFFFFFULL));

    std::vector<std::vector<const TestEntry*>> blocks;
    std::map<std::string, std::size_t> block_index;
    for (const TestEntry* entry : selected) {
        auto it = block_index.find(entry->suite);
        if (it == block_index.end()) {
            block_index.emplace(entry->suite, blocks.size());
            blocks.push_back({entry});
        } else {
            blocks[it->second].push_back(entry);
        }
    }
    std::shuffle(blocks.begin(), blocks.end(), rng);

    std::vector<const TestEntry*> order;
    order.reserve(selected.size());
    for (auto& block : blocks) {
        std::shuffle(block.begin(), block.end(), rng);
        order.insert(order.end(), block.begin(), block.end());
    }
    return order;
}

std::map<std::string, SuiteState> BuildSuiteStates(const std::vector<const TestEntry*>& order) {
    std::map<std::string, SuiteState> states;
    for (const TestEntry* entry : order) {
        if (IsDisabled(entry)) continue;
        SuiteState& state = states[entry->suite];
        ++state.remaining;
        if (!state.setup && entry->suite_setup) state.setup = entry->suite_setup;
        if (!state.teardown && entry->suite_teardown) state.teardown = entry->suite_teardown;
    }
    return states;
}

EntryOutcome RunEntry(const TestEntry* entry, SuiteState& suite_state, bool break_on_failure,
                      JsonReport* report) {
    std::printf("[ RUN      ] %s.%s\n", entry->suite.c_str(), entry->name.c_str());
    std::unique_ptr<TestCase> test = entry->factory();
    g_current_entry.store(entry);
    SetCurrentTest(test.get());
    ClearFatalFailure();

    const auto start = std::chrono::steady_clock::now();
    bool skipped = false;
    bool setup_failed = false;

    if (!suite_state.setup_called && suite_state.setup) {
        suite_state.setup_called = true;
        try {
            suite_state.setup();
        } catch (const SkipException& ex) {
            test->RecordFailure("SetUpTestSuite threw: " + ex.message);
        } catch (const std::exception& ex) {
            test->RecordFailure("SetUpTestSuite threw: " + std::string(ex.what()));
        } catch (...) {
            test->RecordFailure("SetUpTestSuite threw unknown exception");
        }
    }

    try {
        test->SetUp();
    } catch (const SkipException&) {
        skipped = true;
    } catch (const std::exception& ex) {
        test->RecordFailure("SetUp threw: " + std::string(ex.what()));
        setup_failed = true;
    } catch (...) {
        test->RecordFailure("SetUp threw unknown exception");
        setup_failed = true;
    }

    if (!skipped && !setup_failed && !HasFatalFailure()) {
        try {
            test->RunBody();
        } catch (const SkipException&) {
            skipped = true;
        } catch (const std::exception& ex) {
            test->RecordFailure("test body threw: " + std::string(ex.what()));
        } catch (...) {
            test->RecordFailure("test body threw unknown exception");
        }
    }

    const bool abandoned = test->IsAbandoned();
    if (!abandoned) {
        try {
            test->TearDown();
        } catch (const SkipException&) {
            skipped = true;
        } catch (const std::exception& ex) {
            test->RecordFailure("TearDown threw: " + std::string(ex.what()));
        } catch (...) {
            test->RecordFailure("TearDown threw");
        }
    }

    if (--suite_state.remaining == 0 && suite_state.teardown) {
        try {
            suite_state.teardown();
        } catch (const SkipException&) {
        } catch (const std::exception& ex) {
            test->RecordFailure("TearDownTestSuite threw: " + std::string(ex.what()));
        } catch (...) {
            test->RecordFailure("TearDownTestSuite threw unknown exception");
        }
    }

    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const bool failed = test->HasFailures();
    const bool final_skip = skipped && !failed;

    TestResult result;
    result.suite = entry->suite;
    result.name = entry->name;
    result.passed = !failed;
    result.skipped = final_skip;
    result.elapsed_ms = elapsed_ms;
    for (const TestEndListener& listener : TestEndListeners()) {
        try {
            listener(result);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "TestEndListener threw: %s\n", ex.what());
        } catch (...) {
            std::fprintf(stderr, "TestEndListener threw unknown exception\n");
        }
    }

    if (failed) {
        std::printf("[  FAILED  ] %s.%s (%lld ms)\n", entry->suite.c_str(), entry->name.c_str(),
                    elapsed_ms);
        for (const std::string& failure : test->Failures()) {
            std::printf("  %s\n", failure.c_str());
        }
    } else if (final_skip) {
        std::printf("[  SKIPPED ] %s.%s (%lld ms)\n", entry->suite.c_str(), entry->name.c_str(),
                    elapsed_ms);
    } else {
        std::printf("[       OK ] %s.%s (%lld ms)\n", entry->suite.c_str(), entry->name.c_str(),
                    elapsed_ms);
    }

    if (report != nullptr) {
        RecordedTest record;
        record.suite = entry->suite;
        record.name = entry->name;
        record.status = failed ? "FAILED" : (final_skip ? "SKIPPED" : "PASSED");
        record.elapsed_ms = elapsed_ms;
        record.properties = test->Properties();
        if (failed) {
            record.failures = test->Failures();
        }
        report->Append(std::move(record));
    }

    if (abandoned) {
        (void)test.release();
    }
    SetCurrentTest(nullptr);
    g_current_entry.store(nullptr);

    if (failed && break_on_failure) {
        std::fflush(stdout);
        std::fflush(stderr);
        std::abort();
    }

    return EntryOutcome{failed, final_skip};
}

}  // namespace

// ── 用例状态与属性 ──

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

void TestCase::MarkAbandoned() {
    abandoned_.store(true);
}

bool TestCase::IsAbandoned() const {
    return abandoned_.load();
}

void TestCase::RecordProperty(std::string key, std::string value) {
    properties_[std::move(key)] = std::move(value);
}

void TestCase::RecordProperty(std::string key, long long value) {
    properties_[std::move(key)] = std::to_string(value);
}

const std::map<std::string, std::string>& TestCase::Properties() const {
    return properties_;
}

// ── 注册表 ──

Registry& Registry::Instance() {
    static Registry instance;
    return instance;
}

void Registry::Register(std::string suite, std::string name,
                        LifecycleHook suite_setup, LifecycleHook suite_teardown,
                        TestFactory factory) {
    entries_.push_back(TestEntry{std::move(suite), std::move(name), std::move(suite_setup),
                                 std::move(suite_teardown), std::move(factory)});
}

const std::vector<TestEntry>& Registry::Entries() const {
    return entries_;
}

std::vector<const TestEntry*> Registry::Select(std::string_view filter) const {
    std::vector<const TestEntry*> result;
    if (filter.empty()) {
        for (const TestEntry& entry : entries_) result.push_back(&entry);
        return result;
    }
    std::string_view positive = filter;
    std::string_view negative;
    const size_t dash = filter.find('-');
    if (dash != std::string_view::npos) {
        positive = filter.substr(0, dash);
        negative = filter.substr(dash + 1);
    }
    const std::vector<std::string_view> positive_patterns = SplitPatterns(positive);
    const std::vector<std::string_view> negative_patterns = SplitPatterns(negative);
    for (const TestEntry& entry : entries_) {
        if (!positive_patterns.empty() && !MatchAny(entry, positive_patterns)) continue;
        if (MatchAny(entry, negative_patterns)) continue;
        result.push_back(&entry);
    }
    return result;
}

// ── 当前用例归因 ──

void SetCurrentTest(TestCase* test) {
    g_current_test.store(test);
    t_current_test = test;
}

TestCase* CurrentTest() {
    return t_current_test != nullptr ? t_current_test : g_current_test.load();
}

std::string_view CurrentTestName() {
    const TestEntry* e = g_current_entry.load();
    return e ? e->name : std::string_view();
}

std::string_view CurrentSuiteName() {
    const TestEntry* e = g_current_entry.load();
    return e ? e->suite : std::string_view();
}

// ── 断言失败记录 ──

void AssertionFailure(const char* file, int line,
                      std::string_view expr_a, std::string_view expr_b,
                      std::string_view val_a, std::string_view val_b) {
    std::string msg = std::string(file) + ":" + std::to_string(line) +
        ": Failure: Expected " + std::string(expr_a) + " == " + std::string(expr_b) +
        ", got [" + std::string(val_a) + "] vs [" + std::string(val_b) + "]";
    AppendTraceChain(msg);
    if (TestCase* t = CurrentTest()) {
        t->RecordFailure(std::move(msg));
    } else {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

void AssertionFailure(const char* file, int line, std::string_view message) {
    std::string msg = std::string(file) + ":" + std::to_string(line) +
        ": Failure: " + std::string(message);
    AppendTraceChain(msg);
    if (TestCase* t = CurrentTest()) {
        t->RecordFailure(std::move(msg));
    } else {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

// ── 全局环境与监听器 ──

void AddGlobalTestEnvironment(std::unique_ptr<Environment> env) {
    GlobalEnvironments().push_back(std::move(env));
}

void AddTestEndListener(TestEndListener listener) {
    TestEndListeners().push_back(std::move(listener));
}

// ── 运行入口 ──

int RunAll(int argc, char** argv) {
    const ParsedCommandLine parsed = ParseCommandLine(argc, argv);
    if (parsed.exit_now) return parsed.exit_code;
    const Options& options = parsed.options;
    if (options.help) {
        PrintHelp(argv[0]);
        return 0;
    }

    const std::vector<const TestEntry*> selected = Registry::Instance().Select(options.filter);

    if (options.list_tests) {
        for (const TestEntry* entry : selected) {
            std::printf("%s.%s\n", entry->suite.c_str(), entry->name.c_str());
        }
        return 0;
    }
    if (options.gtest_list_tests) {
        std::string current_suite;
        for (const TestEntry* entry : selected) {
            if (entry->suite != current_suite) {
                std::printf("%s.\n", entry->suite.c_str());
                current_suite = entry->suite;
            }
            std::printf("  %s\n", entry->name.c_str());
        }
        return 0;
    }

    const std::vector<const TestEntry*> order = BuildExecutionOrder(selected, options);
    const bool json_requested = !options.json_output_path.empty();

    std::vector<std::unique_ptr<Environment>>& environments = GlobalEnvironments();
    for (const std::unique_ptr<Environment>& environment : environments) {
        try {
            environment->SetUp();
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "Global test environment SetUp threw: %s\n", ex.what());
            return 1;
        } catch (...) {
            std::fprintf(stderr, "Global test environment SetUp threw unknown exception\n");
            return 1;
        }
    }

    JsonReport report;
    int total_failed = 0;
    for (long long round = 0; round < options.repeat; ++round) {
        std::map<std::string, SuiteState> states = BuildSuiteStates(order);
        int ran = 0;
        int failed = 0;
        int skipped = 0;
        std::vector<const TestEntry*> failed_entries;
        const auto round_start = std::chrono::steady_clock::now();
        for (const TestEntry* entry : order) {
            if (IsDisabled(entry)) {
                std::printf("[  SKIPPED ] %s.%s\n", entry->suite.c_str(), entry->name.c_str());
                continue;
            }
            const EntryOutcome outcome = RunEntry(entry, states[entry->suite],
                                                  options.break_on_failure,
                                                  json_requested ? &report : nullptr);
            ++ran;
            if (outcome.failed) {
                ++failed;
                failed_entries.push_back(entry);
            }
            if (outcome.skipped) ++skipped;
        }
        const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - round_start).count();
        std::printf("[==========] %d tests ran. (%lld ms total)\n", ran, total_ms);
        if (skipped > 0) {
            std::printf("[  SKIPPED ] %d tests.\n", skipped);
        }
        const int passed = ran - failed - skipped;
        if (passed > 0 || failed == 0) {
            std::printf("[  PASSED  ] %d tests.\n", passed);
        }
        if (failed > 0) {
            std::printf("[  FAILED  ] %d tests, listed below:\n", failed);
            for (const TestEntry* entry : failed_entries) {
                std::printf("[  FAILED  ] %s.%s\n", entry->suite.c_str(), entry->name.c_str());
            }
        }
        total_failed += failed;
    }

    bool environment_failed = false;
    for (auto it = environments.rbegin(); it != environments.rend(); ++it) {
        try {
            (*it)->TearDown();
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "Global test environment TearDown threw: %s\n", ex.what());
            environment_failed = true;
        } catch (...) {
            std::fprintf(stderr, "Global test environment TearDown threw unknown exception\n");
            environment_failed = true;
        }
    }

    if (json_requested && !WriteJsonReport(options.json_output_path, report)) {
        std::fprintf(stderr, "Failed to write JSON report: %s\n", options.json_output_path.c_str());
        return 1;
    }

    return (total_failed > 0 || environment_failed) ? 1 : 0;
}

}  // namespace mcp::test
