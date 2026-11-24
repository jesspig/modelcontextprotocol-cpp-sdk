#include <mcp/client/SkillVerify.hpp>
#include <mcp/ProtocolVersion.hpp>
#include <mcp/detail/sha256.hpp>

#include <c4/yml/yml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool DecodeBase64Strict(std::string_view text, std::string& out, std::string& error_out) {
    out.clear();
    if (text.size() > kMaxSkillEncodedChars) {
        error_out = "encoded blob exceeds character bound";
        return false;
    }
    if (text.empty()) return true;
    if (text.size() % 4 != 0) {
        error_out = "base64 length is not a multiple of four";
        return false;
    }
    size_t padding = 0;
    if (text.back() == '=') {
        padding = 1;
        if (text.size() >= 2 && text[text.size() - 2] == '=') padding = 2;
    }
    const size_t data_chars = text.size() - padding;
    for (size_t i = 0; i < data_chars; ++i) {
        if (Base64Value(text[i]) < 0) {
            error_out = "base64 contains a character outside the standard alphabet";
            return false;
        }
    }
    for (size_t i = data_chars; i < text.size(); ++i) {
        if (text[i] != '=') {
            error_out = "base64 has data after padding";
            return false;
        }
    }
    if (padding == 1 && (Base64Value(text[data_chars - 1]) & 0x03) != 0) {
        error_out = "base64 is not canonical";
        return false;
    }
    if (padding == 2 && (Base64Value(text[data_chars - 1]) & 0x0f) != 0) {
        error_out = "base64 is not canonical";
        return false;
    }
    const size_t decoded = (text.size() / 4) * 3 - padding;
    if (decoded > kMaxSkillDecodedBytes) {
        error_out = "decoded blob exceeds byte budget";
        return false;
    }
    out.reserve(decoded);
    for (size_t i = 0; i < data_chars; i += 4) {
        uint32_t bits = 0;
        for (size_t j = 0; j < 4; ++j) {
            const int value = i + j < data_chars ? Base64Value(text[i + j]) : 0;
            bits = (bits << 6) | static_cast<uint32_t>(value);
        }
        const size_t available = std::min<size_t>(4, data_chars - i);
        out.push_back(static_cast<char>((bits >> 16) & 0xff));
        if (available >= 3) out.push_back(static_cast<char>((bits >> 8) & 0xff));
        if (available == 4) out.push_back(static_cast<char>(bits & 0xff));
    }
    return true;
}

bool LooksImplicitTyped(std::string_view text) {
    static constexpr std::string_view kBools[] = {
        "true", "false", "yes", "no", "on", "off", "null", "~",
        ".inf", ".nan", "-.inf", "+.inf"};
    std::string lower(ToLower(text));
    for (auto token : kBools) {
        if (lower == token) return true;
    }
    if (lower.size() >= 10 && lower[4] == '-' && lower[7] == '-') {
        bool digits = true;
        for (size_t i = 0; i < lower.size(); ++i) {
            if (i == 4 || i == 7) continue;
            if (!std::isdigit(static_cast<unsigned char>(lower[i]))) { digits = false; break; }
        }
        if (digits) return true;
    }
    size_t i = 0;
    if (i < lower.size() && (lower[i] == '+' || lower[i] == '-')) ++i;
    size_t digits = 0;
    bool dot = false;
    bool exponent = false;
    for (; i < lower.size(); ++i) {
        char c = lower[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { ++digits; continue; }
        if (c == '.' && !dot && !exponent) { dot = true; continue; }
        if ((c == 'e' || c == 'E') && !exponent && digits > 0) { exponent = true; continue; }
        if ((c == '+' || c == '-') && exponent && i > 0 &&
            (lower[i - 1] == 'e' || lower[i - 1] == 'E')) continue;
        return false;
    }
    return digits > 0;
}

std::string ScalarString(c4::csubstr text) {
    return std::string(text.data(), text.size());
}

struct YamlFrame {
    c4::yml::id_type id;
    size_t depth;
};

bool ValidateYamlTree(const c4::yml::Tree& tree, std::string& error_out) {
    const size_t total = static_cast<size_t>(tree.size());
    if (total == 0) {
        error_out = "yaml tree is empty";
        return false;
    }
    if (total > kMaxYamlNodes) {
        error_out = "yaml node budget exceeded";
        return false;
    }
    std::vector<YamlFrame> stack;
    stack.push_back({tree.root_id(), 0});
    while (!stack.empty()) {
        const YamlFrame frame = stack.back();
        stack.pop_back();
        if (frame.depth > kMaxYamlDepth) {
            error_out = "yaml depth budget exceeded";
            return false;
        }
        const c4::yml::id_type id = frame.id;
        if (tree.is_stream(id) || tree.is_doc(id)) {
            error_out = "yaml document count budget exceeded";
            return false;
        }
        if (tree.has_key_anchor(id) || tree.has_val_anchor(id) || tree.is_ref(id)) {
            error_out = "yaml alias is not allowed";
            return false;
        }
        if (tree.has_key_tag(id) || tree.has_val_tag(id)) {
            error_out = "yaml custom tag is not allowed";
            return false;
        }
        if (tree.has_key(id)) {
            const auto key = tree.key(id);
            if (key.size() > kMaxYamlKeyBytes) {
                error_out = "yaml key byte budget exceeded";
                return false;
            }
            if (key == "<<") {
                error_out = "yaml merge key is not allowed";
                return false;
            }
        }
        if (tree.has_val(id) && !tree.has_children(id)) {
            const auto val = tree.val(id);
            if (val.size() > kMaxYamlScalarBytes) {
                error_out = "yaml scalar byte budget exceeded";
                return false;
            }
            if (!tree.is_val_quoted(id) && LooksImplicitTyped(ScalarString(val))) {
                error_out = "yaml implicit scalar conversion is not allowed";
                return false;
            }
        }
        if (tree.has_children(id)) {
            if (tree.is_map(id)) {
                std::vector<std::string> keys;
                for (c4::yml::id_type child = tree.first_child(id);
                     child != c4::yml::NONE; child = tree.next_sibling(child)) {
                    if (tree.has_key(child)) keys.push_back(ScalarString(tree.key(child)));
                }
                std::sort(keys.begin(), keys.end());
                if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
                    error_out = "yaml duplicate key";
                    return false;
                }
            }
            for (c4::yml::id_type child = tree.first_child(id);
                 child != c4::yml::NONE; child = tree.next_sibling(child)) {
                stack.push_back({child, frame.depth + 1});
            }
        }
    }
    return true;
}

JsonValue BuildYamlValue(const c4::yml::Tree& tree, c4::yml::id_type id) {
    if (tree.has_val(id) && !tree.has_children(id)) return JsonValue(ScalarString(tree.val(id)));
    if (tree.is_map(id)) {
        JsonValue obj(JsonValue::object_tag);
        for (c4::yml::id_type child = tree.first_child(id);
             child != c4::yml::NONE; child = tree.next_sibling(child)) {
            if (tree.has_key(child)) {
                obj[ScalarString(tree.key(child))] = BuildYamlValue(tree, child);
            }
        }
        return obj;
    }
    if (tree.is_seq(id)) {
        JsonValue::Array arr;
        for (c4::yml::id_type child = tree.first_child(id);
             child != c4::yml::NONE; child = tree.next_sibling(child)) {
            arr.push_back(BuildYamlValue(tree, child));
        }
        return JsonValue(std::move(arr));
    }
    return JsonValue();
}

bool HasFrontmatterDelimiter(std::string_view document) {
    const size_t end = document.find('\n');
    if (end == std::string_view::npos) return false;
    return document.substr(0, end) == "---";
}

bool PathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rit = root.begin();
    auto cit = candidate.begin();
    for (; rit != root.end(); ++rit, ++cit) {
        if (cit == candidate.end() || !(*rit == *cit)) return false;
    }
    return true;
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

bool ParseFrontmatter(std::string_view yaml, JsonValue& out, std::string& error_out) {
    if (yaml.size() > kMaxSkillFrontmatterBytes) {
        error_out = "yaml byte budget exceeded";
        return false;
    }
    size_t pos = 0;
    while (pos <= yaml.size()) {
        const size_t end = yaml.find('\n', pos);
        const std::string_view line = yaml.substr(pos, (end == std::string_view::npos ? yaml.size() : end) - pos);
        if (!line.empty() && line.front() == '%') {
            error_out = "yaml directive is not allowed";
            return false;
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    std::string owned(yaml);
    c4::yml::Tree tree;
    try {
        tree = c4::yml::parse_in_arena(c4::csubstr(owned.data(), owned.size()));
    } catch (const std::exception&) {
        error_out = "yaml parse failed";
        return false;
    }
    if (!ValidateYamlTree(tree, error_out)) return false;
    out = BuildYamlValue(tree, tree.root_id());
    if (!out.IsObject()) {
        error_out = "yaml frontmatter is not a mapping";
        return false;
    }
    return true;
}

bool SplitSkillDocument(
    std::string_view document, std::string& frontmatter, std::string& body, std::string& error_out)
{
    if (!HasFrontmatterDelimiter(document)) {
        error_out = "frontmatter_absent";
        return false;
    }
    const size_t first_end = document.find('\n');
    size_t pos = first_end + 1;
    while (pos <= document.size()) {
        const size_t end = document.find('\n', pos);
        const size_t line_end = end == std::string_view::npos ? document.size() : end;
        std::string_view line = document.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "---") {
            frontmatter = std::string(document.substr(first_end + 1, pos - first_end - 1));
            body = end == std::string_view::npos
                ? std::string() : std::string(document.substr(end + 1));
            if (frontmatter.size() > kMaxSkillFrontmatterBytes) {
                error_out = "yaml byte budget exceeded";
                return false;
            }
            return true;
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    error_out = "unterminated frontmatter";
    return false;
}

bool VerifyFrontmatterBytes(
    const Skill& advertised, std::string_view document, std::string& error_out)
{
    if (!HasFrontmatterDelimiter(document)) {
        return VerifyFrontmatter(advertised, advertised.frontmatter, error_out);
    }
    std::string frontmatter;
    std::string body;
    if (!SplitSkillDocument(document, frontmatter, body, error_out)) return false;
    JsonValue parsed;
    if (!ParseFrontmatter(frontmatter, parsed, error_out)) return false;
    return VerifyFrontmatter(advertised, parsed, error_out);
}

bool ResolveSkillLocalPath(
    std::string_view root, std::string_view relative, std::string& resolved, std::string& error_out)
{
    namespace fs = std::filesystem;
    if (relative.empty()) {
        error_out = "skill local path is empty";
        return false;
    }
    if (relative.find("://") != std::string_view::npos || relative.front() == '/' ||
        relative.front() == '\\' || (relative.size() >= 2 && relative[1] == ':')) {
        error_out = "absolute skill local path is rejected";
        return false;
    }
    const fs::path relative_path{std::string(relative)};
    for (const auto& part : relative_path) {
        if (part == "..") {
            error_out = "skill local path escapes its root";
            return false;
        }
    }
    std::error_code ec;
    const fs::path root_abs = fs::absolute(fs::path(std::string(root)), ec);
    if (ec) {
        error_out = "skill root cannot be resolved";
        return false;
    }
    const fs::path root_norm = root_abs.lexically_normal();
    const fs::path candidate = (root_norm / relative_path).lexically_normal();
    if (!PathWithin(root_norm, candidate)) {
        error_out = "skill local path escapes its root";
        return false;
    }
    const fs::path root_real = fs::weakly_canonical(root_norm, ec);
    if (ec) {
        error_out = "skill root cannot be canonicalized";
        return false;
    }
    const fs::path candidate_real = fs::weakly_canonical(candidate, ec);
    if (ec || !PathWithin(root_real, candidate_real)) {
        error_out = "skill local path escapes its root through a symbolic link";
        return false;
    }
    resolved = candidate_real.u8string();
    return true;
}

SkillRoute ResolveSkillRoute(
    std::string_view negotiated_version, const ServerCapabilities& capabilities)
{
    if (!IsModernProtocolVersion(negotiated_version)) return SkillRoute::Legacy;
    if (capabilities.extensions &&
        capabilities.extensions->find(std::string(kSkillsExtensionId)) ==
            capabilities.extensions->end()) {
        return SkillRoute::Legacy;
    }
    return SkillRoute::Modern;
}

SkillBlob SkillBlob::FromEncoded(std::string encoded, std::string& error_out) {
    SkillBlob blob;
    if (encoded.size() > kMaxSkillEncodedChars) {
        error_out = "encoded blob exceeds character bound";
        return blob;
    }
    blob.metrics_.raw_size = encoded.size();
    const std::string digest = detail::Sha256::Hash(encoded);
    std::copy(digest.begin(), digest.end(), blob.metrics_.raw_digest.begin());
    blob.encoded_ = std::move(encoded);
    return blob;
}

SkillBlob SkillBlob::FromText(std::string text, std::string& error_out) {
    SkillBlob blob = FromEncoded(std::move(text), error_out);
    if (!error_out.empty()) return blob;
    if (blob.metrics_.raw_size > kMaxSkillDecodedBytes) {
        error_out = "text blob exceeds decoded byte budget";
        return blob;
    }
    blob.metrics_.decoded_size = blob.metrics_.raw_size;
    blob.decoded_ = blob.encoded_;
    return blob;
}

bool SkillBlob::Decode(std::string& error_out) {
    if (decoded_.has_value()) return true;
    std::string out;
    if (!DecodeBase64Strict(encoded_, out, error_out)) return false;
    if (out.size() > kMaxSkillDecodedBytes) {
        error_out = "decoded blob exceeds byte budget";
        return false;
    }
    metrics_.decoded_size = out.size();
    decoded_ = std::move(out);
    return true;
}

bool SkillBlob::Frontmatter(JsonValue& out, std::string& error_out) {
    if (frontmatter_.has_value()) {
        out = *frontmatter_;
        return true;
    }
    if (!Decode(error_out)) return false;
    std::string frontmatter;
    std::string body;
    if (!SplitSkillDocument(*decoded_, frontmatter, body, error_out)) return false;
    JsonValue parsed;
    if (!ParseFrontmatter(frontmatter, parsed, error_out)) return false;
    frontmatter_ = parsed;
    out = parsed;
    return true;
}

bool SkillBlob::Body(std::string& out, std::string& error_out) {
    if (body_.has_value()) {
        out = *body_;
        return true;
    }
    if (!Decode(error_out)) return false;
    std::string frontmatter;
    std::string body;
    if (!SplitSkillDocument(*decoded_, frontmatter, body, error_out)) return false;
    body_ = std::move(body);
    out = *body_;
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
