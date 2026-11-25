#include <mcp/McpError.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/detail/WebSocketFrameParser.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/storage/FileEventStore.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::string Bytes(const std::vector<unsigned char>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}

TEST(StressTest, SlowConsumerAndFullQueueStayBounded) {
    mcp::MessageChannel channel(64);
    for (int index = 0; index < 64; ++index) {
        ASSERT_TRUE(channel.TrySend(mcp::JsonRpcMessage{}));
    }
    EXPECT_FALSE(channel.TrySend(mcp::JsonRpcMessage{}));

    std::atomic<int> received{0};
    std::thread consumer([&] {
        for (int index = 0; index < 64; ++index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            channel.AsyncReceive([&](std::error_code error, mcp::JsonRpcMessage) {
                if (!error) received.fetch_add(1);
            });
        }
    });
    consumer.join();
    EXPECT_EQ(received.load(), 64);
    EXPECT_TRUE(channel.Empty());
    EXPECT_TRUE(channel.TrySend(mcp::JsonRpcMessage{}));
    channel.Close();
    EXPECT_FALSE(channel.IsOpen());
    EXPECT_FALSE(channel.TrySend(mcp::JsonRpcMessage{}));
}

TEST(StressTest, ConcurrentProducersNeverExceedCapacity) {
    mcp::MessageChannel channel(64);
    std::atomic<int> accepted{0};
    std::vector<std::thread> producers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        producers.emplace_back([&] {
            for (int index = 0; index < 200; ++index) {
                if (channel.TrySend(mcp::JsonRpcMessage{})) accepted.fetch_add(1);
            }
        });
    }
    for (auto& producer : producers) producer.join();
    EXPECT_LE(accepted.load(), 64);

    int drained = 0;
    while (!channel.Empty()) {
        channel.AsyncReceive([&](std::error_code error, mcp::JsonRpcMessage) {
            if (!error) ++drained;
        });
    }
    EXPECT_EQ(drained, accepted.load());
}

TEST(StressTest, DeepNestingRejectedAtBoundary) {
    const std::string nested_999 = std::string(999, '[') + std::string(999, ']');
    EXPECT_NO_THROW((void)mcp::JsonValue::Parse(nested_999));
    const std::string nested_1000 = std::string(1000, '[') + std::string(1000, ']');
    EXPECT_NO_THROW((void)mcp::JsonValue::Parse(nested_1000));
    const std::string nested_1001 = std::string(1001, '[') + std::string(1001, ']');
    EXPECT_THROW((void)mcp::JsonValue::Parse(nested_1001), mcp::McpError);
}

TEST(StressTest, MaliciousLargeArrayParsesWithinBudget) {
    std::string array = "[";
    for (int index = 0; index < 200000; ++index) {
        if (index != 0) array += ',';
        array += '1';
    }
    array += ']';
    const auto start = Clock::now();
    const mcp::JsonValue value = mcp::JsonValue::Parse(array);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    ASSERT_TRUE(value.IsArray());
    EXPECT_EQ(value.GetArray().size(), std::size_t{200000});
    EXPECT_LT(elapsed, 2000.0);
}

TEST(StressTest, MalformedWebSocketFramesRejected) {
    const std::vector<std::vector<unsigned char>> malformed = {
        {0x71, 0x00},
        {0x81, 0x80},
        {0x88, 0x7e, 0x00, 0x80},
        {0x08, 0x00},
        {0x82, 0x7f, 0x80, 0, 0, 0, 0, 0, 0, 0},
        {0x82, 0x7f, 0, 0, 0, 1, 0, 0, 0, 0},
        {0x82},
        {0x81, 0x7e, 0x00}
    };
    for (const auto& frame : malformed) {
        EXPECT_THROW((void)mcp::detail::ParseWebSocketFrameHeader(Bytes(frame)), mcp::McpError);
    }
    const std::vector<unsigned char> valid = {0x81, 0x00};
    EXPECT_EQ(mcp::detail::ParseWebSocketFrameHeader(Bytes(valid)).opcode, 1);
}

TEST(StressTest, DisconnectClosesOwnChannelWithoutThrow) {
    const auto pair = mcp::InMemoryTransport::CreatePair();
    ASSERT_NE(pair.client, nullptr);
    ASSERT_NE(pair.server, nullptr);
    EXPECT_TRUE(pair.client->GetMessageChannel().IsOpen());
    pair.client->SendMessageAsync(mcp::JsonRpcMessage{});
    pair.client->Close();
    EXPECT_FALSE(pair.client->GetMessageChannel().IsOpen());
    EXPECT_NO_THROW(pair.client->Close());
    EXPECT_NO_THROW(pair.client->SendMessageAsync(mcp::JsonRpcMessage{}));
    std::error_code error;
    pair.client->GetMessageChannel().AsyncReceive(
        [&](std::error_code received_error, mcp::JsonRpcMessage) {
            EXPECT_TRUE(received_error == std::errc::operation_canceled);
        });
    (void)error;
}

TEST(StressTest, CrashRecoveryPreservesGoodEvents) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mcp-t22-stress-" +
                       std::to_string(Clock::now().time_since_epoch().count()));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    {
        mcp::FileEventStore store(root);
        EXPECT_EQ(store.Append("sess1", "data-0"), 1u);
        EXPECT_EQ(store.Append("sess1", "data-1"), 2u);
    }
    const auto file_path = root / "sess-sess1.jsonl";
    {
        std::ofstream file(file_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(file.is_open());
        file << "{\"id\":3,\"dat";
    }
    {
        mcp::FileEventStore recovered(root);
        const auto events = recovered.GetEventsSince("sess1", 0);
        ASSERT_EQ(events.size(), std::size_t{2});
        EXPECT_EQ(events[0].second, "data-0");
        EXPECT_EQ(events[1].second, "data-1");
        EXPECT_EQ(recovered.Append("sess1", "data-2"), 3u);
        EXPECT_EQ(recovered.GetEventsSince("sess1", 0).size(), std::size_t{3});
    }
    std::filesystem::remove_all(root, cleanup_error);
}
