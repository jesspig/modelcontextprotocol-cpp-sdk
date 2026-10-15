#pragma once
#include "McpApi.hpp"
#include "McpAssert.hpp"
#include "McpTrace.hpp"
#include "McpTestInfo.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TEST(suite, name) \
    class mcp_test_##suite##_##name##_test : public ::mcp::test::TestCase { \
    public: \
        void RunBody() override; \
    private: \
        static const bool kRegistered_; \
    }; \
    const bool mcp_test_##suite##_##name##_test::kRegistered_ = \
        (::mcp::test::Registry::Instance().Register(#suite, #name, {}, {}, \
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
             &::mcp::test::detail::SuiteSetUpHook<suite>::Run, \
             &::mcp::test::detail::SuiteTearDownHook<suite>::Run, \
             []() -> std::unique_ptr<::mcp::test::TestCase> { \
                 return std::make_unique<mcp_test_##suite##_##name##_test>(); \
             }), \
         true); \
    void mcp_test_##suite##_##name##_test::RunBody()
