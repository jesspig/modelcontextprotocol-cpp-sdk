#include <mcp/client/MrtrHelper.hpp>

namespace mcp {

nlohmann::json MrtrDriver::Execute(
    RequestSender send,
    InputResolver resolve,
    nlohmann::json initial_params)
{
    nlohmann::json params = std::move(initial_params);
    std::optional<nlohmann::json> input_responses;
    std::optional<std::string> request_state;

    for (int round = 0; round < max_rounds_; ++round) {
        auto result = send(params, input_responses, request_state);

        if (!IsInputRequired(result)) {
            return result;
        }

        // Parse InputRequiredResult
        InputRequiredResult ir;
        if (auto it = result.find("inputRequests"); it != result.end()) {
            ir.input_requests = it->get<InputRequests>();
        }
        if (auto it = result.find("requestState"); it != result.end() && it->is_string()) {
            ir.request_state = it->get<std::string>();
        }

        // Resolve input requests
        input_responses = resolve(ir.input_requests);
        request_state = ir.request_state;
    }

    // Max rounds exceeded - return the last input_required result
    return send(params, input_responses, request_state);
}

} // namespace mcp
