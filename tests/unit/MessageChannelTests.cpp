// MessageChannelTests — backpressure, non-blocking TrySend, and close wake-up
// semantics of the bounded message queue shared by all transports.

#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/JsonRpc.hpp>

#include <mcp/test/McpTest.hpp>

#include <chrono>
#include <future>
#include <thread>

using namespace mcp;

namespace {

JsonRpcMessage MakeMessage(int64_t seq) {
    JsonRpcRequest req;
    req.id = seq;
    req.method = "ping";
    return JsonRpcMessage{std::move(req)};
}

} // namespace

// A full channel rejects TrySend without blocking.
TEST(MessageChannelTest, TrySendFailsWhenFull) {
    MessageChannel ch(1);
    EXPECT_TRUE(ch.TrySend(MakeMessage(1)));
    EXPECT_FALSE(ch.TrySend(MakeMessage(2)));
    ch.Close();
}

// A consumer draining one slot lets a blocked Send complete; the message is
// delivered in FIFO order.
TEST(MessageChannelTest, FullSendBlocksUntilConsumerDrains) {
    MessageChannel ch(1);
    ASSERT_TRUE(ch.Send(MakeMessage(1)));

    std::promise<bool> result;
    auto future = result.get_future();
    std::thread producer([&] { result.set_value(ch.Send(MakeMessage(2))); });

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    JsonRpcMessage msg;
    ch.AsyncReceive(
        [&msg](std::error_code ec, JsonRpcMessage m) {
            ASSERT_FALSE(ec);
            msg = std::move(m);
        });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_TRUE(future.get());
    EXPECT_EQ(std::get<JsonRpcRequest>(msg).id, RequestId{int64_t(1)});
    producer.join();
    ch.Close();
}

// Close wakes a Send blocked on a full channel; the message is dropped.
TEST(MessageChannelTest, CloseWakesWaitingSend) {
    MessageChannel ch(1);
    ASSERT_TRUE(ch.Send(MakeMessage(1)));

    std::promise<bool> result;
    auto future = result.get_future();
    std::thread producer([&] { result.set_value(ch.Send(MakeMessage(2))); });

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    ch.Close();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_FALSE(future.get());
    producer.join();
}

// Close wakes a receive waiting on an empty channel with operation_canceled.
TEST(MessageChannelTest, CloseWakesWaitingReceive) {
    MessageChannel ch(1);

    std::promise<std::error_code> result;
    auto future = result.get_future();
    std::thread consumer(
        [&] {
            ch.AsyncReceive(
                [&result](std::error_code ec, JsonRpcMessage) { result.set_value(ec); });
        });

    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    ch.Close();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(future.get(), std::make_error_code(std::errc::operation_canceled));
    consumer.join();
}

// Sending after Close drops the message and returns false.
TEST(MessageChannelTest, SendAfterCloseDropsMessage) {
    MessageChannel ch(1);
    ch.Close();
    EXPECT_FALSE(ch.Send(MakeMessage(1)));
    EXPECT_FALSE(ch.TrySend(MakeMessage(2)));
    EXPECT_FALSE(ch.IsOpen());
}
