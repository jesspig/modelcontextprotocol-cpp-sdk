#include <mcp/client/SkillVerify.hpp>
#include <mcp/detail/sha256.hpp>

#include <cctype>
#include <variant>

namespace mcp {

namespace {

std::string ToHex(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

std::string ToLower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string SkillNameSegment(const std::string& uri) {
    constexpr std::string_view kSuffix = "/SKILL.md";
    std::string path = uri.substr(0, uri.size() - kSuffix.size());
    auto scheme_end = path.find("://");
    std::string_view rest = scheme_end == std::string::npos
        ? std::string_view(path)
        : std::string_view(path).substr(scheme_end + 3);
    auto slash = rest.rfind('/');
    std::string_view segment = slash == std::string_view::npos ? rest : rest.substr(slash + 1);
    return std::string(segment);
}

bool SkillResourcesEqual(const SkillResources& a, const SkillResources& b) {
    if (a.index() != b.index()) return false;
    if (auto* av = std::get_if<std::vector<SkillResource>>(&a)) {
        const auto& bv = std::get<std::vector<SkillResource>>(b);
        if (av->size() != bv.size()) return false;
        for (size_t i = 0; i < av->size(); ++i) {
            if ((*av)[i].uri != bv[i].uri) return false;
            if ((*av)[i].digest != bv[i].digest) return false;
            if ((*av)[i].size != bv[i].size) return false;
        }
        return true;
    }
    return std::get<std::string>(a) == std::get<std::string>(b);
}

} // namespace

bool CheckSkillLimits(const Skill& skill, std::string& error_out) {
    auto* manifest = std::get_if<std::vector<SkillResource>>(&skill.resources);
    if (!manifest) return true;
    if (manifest->size() > kMaxSkillFiles) {
        error_out = "skill exceeds file count limit: " + std::to_string(manifest->size());
        return false;
    }
    int64_t total = 0;
    for (const auto& file : *manifest) {
        if (file.size < 0) {
            error_out = "skill file has negative size: " + file.uri;
            return false;
        }
        total += file.size;
        if (total > kMaxSkillTotalBytes) {
            error_out = "skill exceeds total byte limit: " + std::to_string(total);
            return false;
        }
    }
    return true;
}

bool VerifyFileBytes(const SkillResource& expected, std::string_view bytes, std::string& error_out) {
    if (expected.size < 0 || static_cast<uint64_t>(expected.size) != bytes.size()) {
        error_out = "skill file size mismatch: " + expected.uri;
        return false;
    }
    constexpr std::string_view kPrefix = "sha256:";
    if (expected.digest.size() <= kPrefix.size() ||
        ToLower(std::string_view(expected.digest).substr(0, kPrefix.size())) != kPrefix) {
        error_out = "skill file digest is not sha256: " + expected.uri;
        return false;
    }
    std::string actual = ToHex(detail::Sha256::Hash(bytes));
    if (ToLower(std::string_view(expected.digest).substr(kPrefix.size())) != actual) {
        error_out = "skill file digest mismatch: " + expected.uri;
        return false;
    }
    return true;
}

bool VerifyFrontmatter(const Skill& advertised, const JsonValue& fetched_frontmatter, std::string& error_out) {
    if (!(advertised.frontmatter == fetched_frontmatter)) {
        error_out = "skill frontmatter mismatch: " + advertised.uri;
        return false;
    }
    if (!advertised.frontmatter.IsObject()) {
        error_out = "skill frontmatter is not an object: " + advertised.uri;
        return false;
    }
    auto* name = advertised.frontmatter.Find("name");
    if (!name || !name->IsString()) {
        error_out = "skill frontmatter has no string name: " + advertised.uri;
        return false;
    }
    if (name->GetString() != SkillNameSegment(advertised.uri)) {
        error_out = "skill frontmatter name does not match uri: " + advertised.uri;
        return false;
    }
    return true;
}

bool IsApprovalStillValid(const Skill& approved, const Skill& current) {
    if (approved.uri != current.uri) return false;
    if (!(approved.frontmatter == current.frontmatter)) return false;
    if (!(approved.meta == current.meta)) return false;
    return SkillResourcesEqual(approved.resources, current.resources);
}

std::string ApprovalKey(const std::string& host_label, const Skill& skill) {
    std::string key = host_label;
    key.push_back('\x1f');
    key.append(skill.uri);
    return key;
}

} // namespace mcp
