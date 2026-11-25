#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

constexpr size_t kMaxSkillFiles = 512;
constexpr int64_t kMaxSkillTotalBytes = 16 * 1024 * 1024;
constexpr size_t kMaxSkillEncodedChars = 1398104;
constexpr size_t kMaxSkillDecodedBytes = 1048576;
constexpr size_t kMaxSkillFrontmatterBytes = 1048576;
constexpr size_t kMaxYamlDepth = 64;
constexpr size_t kMaxYamlNodes = 10000;
constexpr size_t kMaxYamlScalarBytes = 65536;
constexpr size_t kMaxYamlKeyBytes = 1024;

enum class SkillRoute { Modern, Legacy };

struct SkillBlobMetrics {
    size_t raw_size{0};
    std::array<uint8_t, 32> raw_digest{};
    size_t decoded_size{0};
};

class SkillBlob {
public:
    static SkillBlob FromEncoded(std::string encoded, std::string& error_out);
    static SkillBlob FromText(std::string text, std::string& error_out);
    [[nodiscard]] bool Decode(std::string& error_out);
    [[nodiscard]] bool Frontmatter(JsonValue& out, std::string& error_out);
    [[nodiscard]] bool Body(std::string& out, std::string& error_out);
    [[nodiscard]] const SkillBlobMetrics& Metrics() const { return metrics_; }
    [[nodiscard]] const std::string& Decoded() const { return *decoded_; }
    [[nodiscard]] bool IsDecoded() const { return decoded_.has_value(); }
    [[nodiscard]] bool HasFrontmatter() const { return frontmatter_.has_value(); }
    [[nodiscard]] bool HasBody() const { return body_.has_value(); }

private:
    std::string encoded_;
    SkillBlobMetrics metrics_{};
    std::optional<std::string> decoded_;
    std::optional<JsonValue> frontmatter_;
    std::optional<std::string> body_;
};

[[nodiscard]] SkillRoute ResolveSkillRoute(
    std::string_view negotiated_version, const ServerCapabilities& capabilities);
[[nodiscard]] bool CheckSkillLimits(const Skill& skill, std::string& error_out);
[[nodiscard]] bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out);
[[nodiscard]] bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out);
[[nodiscard]] bool ParseFrontmatter(std::string_view yaml, JsonValue& out, std::string& error_out);
[[nodiscard]] bool SplitSkillDocument(
    std::string_view document, std::string& frontmatter, std::string& body, std::string& error_out);
[[nodiscard]] bool VerifyFrontmatterBytes(
    const Skill& advertised, std::string_view document, std::string& error_out);
[[nodiscard]] bool ResolveSkillLocalPath(
    std::string_view root, std::string_view relative, std::string& resolved, std::string& error_out);
[[nodiscard]] bool IsApprovalStillValid(const Skill& approved, const Skill& current);
std::string ApprovalKey(const std::string& host_label, const Skill& skill);

} // namespace mcp
