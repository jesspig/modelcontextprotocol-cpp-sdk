// MessageFilterTests — unit tests for MessageFilter pipeline chain-of-responsibility

#include <mcp/protocol/MessageFilter.hpp>
#include <mcp/JsonRpc.hpp>

#include <mcp/test/McpTest.hpp>

using namespace mcp;

TEST(MessageFilterTest, EmptyPipeline) {
    FilterPipeline pipeline;
    bool called = false;
    JsonRpcMessage msg;
    pipeline.Execute(msg, [&called](const JsonRpcMessage&) { called = true; });
    EXPECT_TRUE(called);
}

TEST(MessageFilterTest, SingleFilter) {
    FilterPipeline pipeline;
    bool filter_called = false;
    bool final_called = false;
    pipeline.AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [&filter_called](const JsonRpcMessage& msg, MessageFilterNext next) {
            filter_called = true;
            next(msg);
        }));
    JsonRpcMessage msg;
    pipeline.Execute(msg,
                     [&final_called](const JsonRpcMessage&) { final_called = true; });
    EXPECT_TRUE(filter_called);
    EXPECT_TRUE(final_called);
}

TEST(MessageFilterTest, MultipleFiltersOrder) {
    FilterPipeline pipeline;
    std::vector<int> order;
    for (int i = 0; i < 3; i++) {
        pipeline.AddFilter(std::make_shared<MessageFilterFuncAdapter>(
            [i, &order](const JsonRpcMessage& msg, MessageFilterNext next) {
                order.push_back(i);
                next(msg);
            }));
    }
    JsonRpcMessage msg;
    bool final_called = false;
    pipeline.Execute(msg,
                     [&final_called](const JsonRpcMessage&) { final_called = true; });
    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 2);
    EXPECT_TRUE(final_called);
}

TEST(MessageFilterTest, FilterStopsChain) {
    FilterPipeline pipeline;
    bool second_called = false;
    pipeline.AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [](const JsonRpcMessage&, MessageFilterNext) {
            // Intentionally do NOT call next
        }));
    pipeline.AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [&second_called](const JsonRpcMessage&, MessageFilterNext next) {
            second_called = true;
            next(JsonRpcMessage{});
        }));
    JsonRpcMessage msg;
    bool final_called = false;
    pipeline.Execute(msg,
                     [&final_called](const JsonRpcMessage&) { final_called = true; });
    EXPECT_FALSE(second_called);
    EXPECT_FALSE(final_called);
}

TEST(MessageFilterTest, FilterModifiesMessage) {
    FilterPipeline pipeline;
    pipeline.AddFilter(std::make_shared<MessageFilterFuncAdapter>(
        [](const JsonRpcMessage&, MessageFilterNext next) {
            JsonRpcRequest req;
            req.id = int64_t(1);
            req.method = "ping";
            next(JsonRpcMessage{std::move(req)});
        }));
    JsonRpcNotification notif;
    notif.method = "test";
    JsonRpcMessage original{std::move(notif)};
    JsonRpcMessage received;
    pipeline.Execute(original,
                     [&received](const JsonRpcMessage& msg) { received = msg; });
    EXPECT_TRUE(IsRequest(received));
    EXPECT_FALSE(IsNotification(received));
}
