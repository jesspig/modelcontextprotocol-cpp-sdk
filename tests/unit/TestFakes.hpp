#pragma once
// TestFakes.hpp — 手写调用记录型测试替身（不依赖 gmock），供单元测试注入并检查传输行为

#include <mcp/Transport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/test/McpApi.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class FakeTransport : public mcp::ITransport {
public:
    struct SentCall {
        std::string payload;
    };

    std::string_view SessionId() const override { return session_id_; }
    mcp::MessageChannel& GetMessageChannel() override { return channel_; }

    void SendMessageAsync(mcp::JsonRpcMessage message) override {
        sent_.push_back(SentCall{mcp::SerializeMessage(std::move(message))});
    }

    void Close() override {
        closed_ = true;
        channel_.Close();
    }

    bool IsStateless() const override { return is_stateless_; }
    void Start() override { started_ = true; }

    const std::vector<SentCall>& Sent() const { return sent_; }

    const std::string& LastSent() const {
        static const std::string kEmpty;
        return sent_.empty() ? kEmpty : sent_.back().payload;
    }

    bool PushIncoming(mcp::JsonRpcMessage message) {
        return channel_.TrySend(std::move(message));
    }
    bool PushIncoming(std::string payload) {
        return PushIncoming(mcp::DeserializeMessage(payload));
    }

    void SetConnectResult(bool ok) { connect_result_ = ok; }
    bool ConnectResult() const { return connect_result_; }
    void SetSessionId(std::string session_id) { session_id_ = std::move(session_id); }
    bool Closed() const { return closed_; }
    bool Started() const { return started_; }

private:
    mcp::MessageChannel channel_;
    std::string session_id_;
    std::vector<SentCall> sent_;
    bool connect_result_ = true;
    bool is_stateless_ = false;
    bool closed_ = false;
    bool started_ = false;
};

#define EXPECT_CALL_COUNT(fake, n) \
    do { \
        if ((fake).Sent().size() != static_cast<std::size_t>(n)) \
            ::mcp::test::AssertionFailure(__FILE__, __LINE__, \
                std::string("expected send count ") + std::to_string(n) + ", got " + std::to_string((fake).Sent().size())); \
    } while (0)
