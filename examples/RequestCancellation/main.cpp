#include <mcp/client/McpClient.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/protocol/WireCodec.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace mcp;

namespace {

constexpr char kSlowMethod[] = "demo/slow";

class DemoCodec : public WireCodec {
public:
    explicit DemoCodec(std::string_view version)
        : inner_(MakeWireCodec(version))
    {
    }

    bool HasRequestMethod(std::string_view method) const override {
        return IsCustom(method) || inner_->HasRequestMethod(method);
    }

    bool HasNotificationMethod(std::string_view method) const override {
        return IsCustom(method) || inner_->HasNotificationMethod(method);
    }

    WireValidation ValidateRequest(
        std::string_view method, const JsonValue& raw) const override {
        return IsCustom(method)
            ? WireValidation::Ok
            : inner_->ValidateRequest(method, raw);
    }

    WireValidation ValidateResponse(
        std::string_view method, const JsonValue& raw) const override {
        return inner_->ValidateResponse(method, raw);
    }

    WireValidation ValidateNotification(
        std::string_view method, const JsonValue& raw) const override {
        return IsCustom(method)
            ? WireValidation::Ok
            : inner_->ValidateNotification(method, raw);
    }

    void StampOutgoingRequest(
        JsonValue& body, const RequestMeta& meta) const override {
        inner_->StampOutgoingRequest(body, meta);
    }

    JsonValue EncodeResult(
        std::string_view method, const JsonValue& result) const override {
        return inner_->EncodeResult(method, result);
    }

    int32_t EncodeErrorCode(int32_t code) const override {
        return inner_->EncodeErrorCode(code);
    }

    std::string_view Era() const override {
        return inner_->Era();
    }

private:
    static bool IsCustom(std::string_view method) {
        return method == kSlowMethod;
    }

    std::unique_ptr<WireCodec> inner_;
};

RequestHandler InitializeHandler() {
    return [](const JsonRpcRequest&, std::promise<JsonValue> promise) {
        JsonValue result(JsonValue::object_tag);
        result["protocolVersion"] = JsonValue(std::string(kLegacyProtocolVersion));
        result["capabilities"] = SerializeServerCapabilities(ServerCapabilities{});
        result["serverInfo"] =
            SerializeImplementation(Implementation{"CancellationDemo", "1.0"});
        promise.set_value(std::move(result));
    };
}

std::string RequestIdText(const RequestId& id) {
    return std::visit([](const auto& value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            return value;
        } else {
            return std::to_string(value);
        }
    }, id);
}

void PrintOutcome(const RequestOutcome& outcome) {
    if (const auto* value = std::get_if<JsonValue>(&outcome)) {
        std::cout << "result: " << value->Dump() << std::endl;
        return;
    }
    const auto& error = std::get<ErrorData>(outcome);
    std::cout << "error: " << error.message
              << " (code " << static_cast<int32_t>(error.code) << ")"
              << std::endl;
}

} // namespace

int main() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = std::make_shared<McpSessionHandler>(
        std::move(pair.server),
        std::make_unique<DemoCodec>(kLegacyProtocolVersion));

    server->SetRequestHandler(methods::kInitialize, InitializeHandler());

    std::vector<std::shared_ptr<std::promise<JsonValue>>> held_requests;
    server->SetRequestHandler(kSlowMethod,
        [&held_requests](const JsonRpcRequest&, std::promise<JsonValue> promise) {
            held_requests.push_back(
                std::make_shared<std::promise<JsonValue>>(std::move(promise)));
        });
    server->Start();

    ClientOptions client_options;
    client_options.connect_mode = ConnectMode::Legacy;
    auto client = McpClient::Create(std::move(pair.client), client_options);

    RequestOptions timeout_options;
    timeout_options.timeout = std::chrono::milliseconds(200);
    auto timed = client->SendRequest(
        kSlowMethod, JsonValue(JsonValue::object_tag), timeout_options);
    std::cout << "timeout handle id: " << RequestIdText(timed.id) << std::endl;
    PrintOutcome(timed.future.get());

    auto cancelled = client->SendRequest(
        kSlowMethod, JsonValue(JsonValue::object_tag), RequestOptions{});
    std::cout << "cancel handle id: " << RequestIdText(cancelled.id) << std::endl;
    cancelled.Cancel("user requested cancellation");
    PrintOutcome(cancelled.future.get());

    client->Close();
    server->Close();
    return 0;
}
