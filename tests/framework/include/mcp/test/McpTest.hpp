#pragma once
// McpTest.hpp — 自研测试框架：注册、用例基类与运行入口

#include "McpApi.hpp"
#include "McpAssert.hpp"
#include "McpTestInfo.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ── TEST / TEST_F 注册宏（唯一类名拼接，对齐 gtest 模式）──
#define TEST(suite, name) \
    class mcp_test_##suite##_##name##_test : public ::mcp::test::TestCase { \
    public: \
        void RunBody() override; \
    private: \
        static const bool kRegistered_; \
    }; \
    const bool mcp_test_##suite##_##name##_test::kRegistered_ = \
        (::mcp::test::Registry::Instance().Register(#suite, #name, \
             []() -> std::unique_ptr<::mcp::test::TestCase> { \
                 return std::make_unique<mcp_test_##suite##_##name##_test>(); \
             }), \
         true); \
    void mcp_test_##suite##_##name##_test::RunBody()

#define TEST_F(suite, name) \
    class mcp_test_##suite##_##name##_test : public suite { \
    public: \
        void RunBody() override; \
    private: \
        static const bool kRegistered_; \
    }; \
    const bool mcp_test_##suite##_##name##_test::kRegistered_ = \
        (::mcp::test::Registry::Instance().Register(#suite, #name, \
             []() -> std::unique_ptr<::mcp::test::TestCase> { \
                 return std::make_unique<mcp_test_##suite##_##name##_test>(); \
             }), \
         true); \
    void mcp_test_##suite##_##name##_test::RunBody()
