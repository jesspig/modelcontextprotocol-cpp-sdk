#pragma once

#include <mcp/detail/sha256.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

enum class SessionBindingState : int {
    Stateless = 0,
    Sessionful = 1,
};

class ServerRequestContext {
public:
    ServerRequestContext() = default;

    [[nodiscard]] static ServerRequestContext Stateless(
        std::string principal, std::vector<std::string> scopes)
    {
        ServerRequestContext ctx;
        ctx.binding_ = SessionBindingState::Stateless;
        ctx.principal_ = std::move(principal);
        ctx.scopes_ = std::move(scopes);
        return ctx;
    }

    [[nodiscard]] static ServerRequestContext Sessionful(
        std::string session_id, std::string principal,
        std::vector<std::string> scopes)
    {
        ServerRequestContext ctx;
        ctx.binding_ = SessionBindingState::Sessionful;
        ctx.session_id_ = std::move(session_id);
        ctx.principal_ = std::move(principal);
        ctx.scopes_ = std::move(scopes);
        return ctx;
    }

    [[nodiscard]] SessionBindingState Binding() const { return binding_; }
    [[nodiscard]] bool IsSessionful() const {
        return binding_ == SessionBindingState::Sessionful;
    }
    [[nodiscard]] bool IsStateless() const {
        return binding_ == SessionBindingState::Stateless;
    }
    [[nodiscard]] bool IsBound() const { return !principal_.empty(); }
    [[nodiscard]] std::string_view SessionId() const { return session_id_; }
    [[nodiscard]] std::string_view Principal() const { return principal_; }
    [[nodiscard]] const std::vector<std::string>& Scopes() const { return scopes_; }

    [[nodiscard]] std::string RedactedSession() const {
        if (!IsSessionful()) return "stateless";
        return "sid-" + HexPrefix(detail::Sha256::Hash(session_id_));
    }

    [[nodiscard]] std::string CorrelationId() const {
        const std::string seed = std::to_string(static_cast<int>(binding_)) +
            "|" + session_id_ + "|" + principal_;
        return "cid-" + HexPrefix(detail::Sha256::Hash(seed));
    }

private:
    [[nodiscard]] static std::string HexPrefix(std::string_view raw) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(12);
        for (size_t i = 0; i < raw.size() && out.size() < 12; ++i) {
            auto byte = static_cast<unsigned char>(raw[i]);
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
        return out;
    }

    SessionBindingState binding_{SessionBindingState::Stateless};
    std::string session_id_;
    std::string principal_;
    std::vector<std::string> scopes_;
};

} // namespace mcp
