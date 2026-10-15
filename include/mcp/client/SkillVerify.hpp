#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace mcp {

constexpr size_t kMaxSkillFiles = 512;
constexpr int64_t kMaxSkillTotalBytes = 16 * 1024 * 1024;

bool CheckSkillLimits(const Skill& skill, std::string& error_out);
bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out);
bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out);
bool IsApprovalStillValid(const Skill& approved, const Skill& current);
std::string ApprovalKey(const std::string& host_label, const Skill& skill);

} // namespace mcp
