#include "SchemaValidation.hpp"
#include "JsonSchemaValidator.hpp"
#include "schema/ExactNumber.hpp"

#include <re2/re2.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp { namespace detail {

struct AnchorEntry {
    const JsonValue* node = nullptr;
    const JsonValue* resource = nullptr;
    bool dynamic = false;
};

struct ResourceEntry {
    const JsonValue* root = nullptr;
    std::string id;
};

struct RegexHolder {
    std::shared_ptr<RE2> expression;
};

struct CompiledSchema::Impl {
    SchemaOptions options;
    std::deque<JsonValue> documents;
    std::map<std::string, ResourceEntry> resources;
    std::map<std::string, AnchorEntry> anchors;
    std::map<const JsonValue*, std::map<std::string, const JsonValue*>> dynamic_anchors;
    std::map<const void*, RegexHolder> regexes;
    const JsonValue* root = nullptr;
    std::string root_id;
    bool ready = false;
};

namespace schema {

[[noreturn]] void SchemaFail(const char* classifier, const std::string& message,
                             const std::string& location) {
    throw McpError(McpErrorCode::InvalidParams,
        std::string(classifier) + ": " + message + " at " + location);
}

std::string NormalizeUri(std::string uri) {
    if (!uri.empty() && uri.back() == '#') uri.pop_back();
    size_t scheme = uri.find("://");
    if (scheme == std::string::npos) return uri;
    size_t path_start = uri.find('/', scheme + 3);
    if (path_start == std::string::npos) return uri;
    std::string prefix = uri.substr(0, path_start);
    std::string path = uri.substr(path_start);
    std::vector<std::string> segments;
    size_t start = 1;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        std::string segment = path.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (segment == ".") {
            if (end == std::string::npos && path.back() == '/') segments.emplace_back();
        } else if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
            if (end == std::string::npos && path.back() == '/') segments.emplace_back();
        } else {
            segments.push_back(segment);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string result = prefix;
    for (const auto& segment : segments) {
        result.push_back('/');
        result += segment;
    }
    if (path.size() > 1 && path.back() == '/' && !result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    return result;
}

bool HasUriScheme(const std::string& uri) {
    if (uri.empty() || !((uri[0] >= 'A' && uri[0] <= 'Z') || (uri[0] >= 'a' && uri[0] <= 'z'))) {
        return false;
    }
    for (size_t i = 1; i < uri.size(); ++i) {
        char c = uri[i];
        if (c == ':') return i > 1;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '+' || c == '-' || c == '.')) return false;
    }
    return false;
}

std::string JoinUri(const std::string& base, const std::string& reference) {
    if (HasUriScheme(reference)) return reference;
    if (reference.empty()) return base;
    if (reference[0] == '/') {
        size_t scheme = base.find("://");
        if (scheme == std::string::npos) return reference;
        size_t slash = base.find('/', scheme + 3);
        if (slash == std::string::npos) return reference;
        return base.substr(0, slash) + reference;
    }
    size_t slash = base.rfind('/');
    if (slash == std::string::npos) return reference;
    return base.substr(0, slash + 1) + reference;
}

void UpdateResourceContext(const JsonValue& schema, const JsonValue*& resource,
                           std::string& resource_id) {
    if (!schema.IsObject() || resource == &schema) return;
    const JsonValue* id = schema.Find("$id");
    if (!id || !id->IsString()) return;
    resource = &schema;
    resource_id = NormalizeUri(JoinUri(resource_id, id->GetString()));
}

std::string PercentDecode(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int high = hex(text[i + 1]);
            int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

std::string JsonPointerToken(std::string_view token) {
    std::string key;
    key.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '1') key.push_back('/');
            else if (token[i + 1] == '0') key.push_back('~');
            else key.push_back(token[i + 1]);
            ++i;
        } else {
            key.push_back(token[i]);
        }
    }
    return key;
}

const JsonValue* ResolveJsonPointerLocal(const JsonValue& root, std::string_view pointer,
                                          const std::string& location) {
    const JsonValue* current = &root;
    size_t pos = 0;
    while (true) {
        size_t slash = pointer.find('/', pos);
        std::string_view token = slash == std::string_view::npos
            ? pointer.substr(pos) : pointer.substr(pos, slash - pos);
        std::string key = JsonPointerToken(token);
        if (current->IsObject()) {
            const JsonValue* next = current->Find(key);
            if (!next) SchemaFail("SL.UnresolvedRef", "unresolvable JSON pointer", location);
            current = next;
        } else if (current->IsArray()) {
            size_t index = 0;
            bool digits = !key.empty();
            for (char c : key) digits = digits && c >= '0' && c <= '9';
            if (!digits) SchemaFail("SL.UnresolvedRef", "unresolvable array pointer", location);
            for (char c : key) {
                index = index * 10 + static_cast<size_t>(c - '0');
                if (index > 100000) SchemaFail("SL.UnresolvedRef", "array pointer beyond budget", location);
            }
            if (index >= current->Size()) SchemaFail("SL.UnresolvedRef", "array pointer out of range", location);
            current = &current->GetArray()[index];
        } else {
            SchemaFail("SL.UnresolvedRef", "unresolvable JSON pointer", location);
        }
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return current;
}

struct EvalResult {
    bool valid = true;
    std::set<std::string> properties;
    std::set<size_t> items;
};

struct DynamicFrame {
    const JsonValue* resource = nullptr;
    std::string resource_id;
};

struct EvalContext {
    const SchemaBudgets& budgets;
    bool assert_format;
    const std::map<const void*, RegexHolder>& regexes;
    const std::map<std::string, AnchorEntry>& anchors;
    const std::map<std::string, ResourceEntry>& resources;
    const std::map<const JsonValue*, std::map<std::string, const JsonValue*>>& dynamic_anchors;
    size_t steps = 0;
    size_t expand_nodes = 0;
    size_t reference_depth = 0;
    std::chrono::steady_clock::time_point started;
    std::vector<DynamicFrame> dynamic_scope;
    std::set<std::pair<const void*, std::string>> active;

    explicit EvalContext(const CompiledSchema::Impl& impl)
        : budgets(impl.options.budgets),
          assert_format(impl.options.assert_format),
          regexes(impl.regexes),
          anchors(impl.anchors),
          resources(impl.resources),
          dynamic_anchors(impl.dynamic_anchors),
          started(std::chrono::steady_clock::now()) {}
};

struct DynamicScopeGuard {
    EvalContext& context;
    bool pushed;

    DynamicScopeGuard(EvalContext& ctx, const JsonValue* resource,
                      const std::string& resource_id, bool should_push)
        : context(ctx), pushed(should_push) {
        if (pushed) context.dynamic_scope.push_back(DynamicFrame{resource, resource_id});
    }

    DynamicScopeGuard(const DynamicScopeGuard&) = delete;
    DynamicScopeGuard& operator=(const DynamicScopeGuard&) = delete;

    ~DynamicScopeGuard() {
        if (pushed) context.dynamic_scope.pop_back();
    }
};

void MergeResult(EvalResult& target, const EvalResult& source) {
    target.properties.insert(source.properties.begin(), source.properties.end());
    target.items.insert(source.items.begin(), source.items.end());
}

size_t ReadCount(const JsonValue& value, const std::string& location, size_t limit) {
    if (!value.IsNumber()) SchemaFail("SL.InvalidKeyword", "expected non-negative integer", location);
    return static_cast<size_t>(ExactNumber::FromJson(value).ToBoundedInteger(limit));
}

ExactNumber ReadNumber(const JsonValue& value, const std::string& location) {
    if (!value.IsNumber()) SchemaFail("SL.InvalidKeyword", "expected number", location);
    return ExactNumber::FromJson(value);
}

bool ReadBoolean(const JsonValue& value, const std::string& location) {
    if (!value.IsBool()) SchemaFail("SL.InvalidKeyword", "expected boolean", location);
    return value.GetBool();
}

std::string TranslateEcmaPattern(std::string_view pattern) {
    std::string result;
    result.reserve(pattern.size());
    size_t pos = 0;
    while (pos < pattern.size()) {
        bool negated = false;
        size_t prefix = 1;
        if (pattern[pos] == '\\' && pos + 1 < pattern.size()
            && (pattern[pos + 1] == 'p' || pattern[pos + 1] == 'P')) {
            negated = pattern[pos + 1] == 'P';
            prefix = 2;
        }
        if (prefix == 2 && pos + prefix < pattern.size() && pattern[pos + prefix] == '{') {
            size_t close = pattern.find('}', pos + prefix + 1);
            if (close != std::string_view::npos) {
                std::string_view name = pattern.substr(pos + prefix + 1, close - pos - prefix - 1);
                const char* mapped = nullptr;
                if (name == "Letter" || name == "Alphabetic") mapped = "L";
                else if (name == "digit" || name == "Decimal_Number") mapped = "Nd";
                else if (name == "Number") mapped = "N";
                else if (name == "White_Space") mapped = "Z";
                if (mapped) {
                    result.push_back('\\');
                    result.push_back(negated ? 'P' : 'p');
                    result.push_back('{');
                    result += mapped;
                    result.push_back('}');
                    pos = close + 1;
                    continue;
                }
            }
        }
        result.push_back(pattern[pos]);
        ++pos;
    }
    return result;
}

const RE2& GetRegex(const std::string& pattern, EvalContext& ctx, const std::string& location) {
    if (pattern.size() > ctx.budgets.max_pattern_bytes) {
        SchemaFail("SL.RegexPatternBytes", "pattern exceeds byte budget", location);
    }
    auto found = ctx.regexes.find(static_cast<const void*>(pattern.data()));
    if (found == ctx.regexes.end()) {
        SchemaFail("SL.RegexPatternBytes", "pattern was not compiled", location);
    }
    if (!found->second.expression || !found->second.expression->ok()) {
        SchemaFail("SL.RegexFailOpen", "invalid regular expression", location);
    }
    return *found->second.expression;
}

bool RegexMatches(const RE2& expression, const std::string& subject,
                  EvalContext& ctx, const std::string& location) {
    if (subject.size() > ctx.budgets.max_subject_bytes) {
        SchemaFail("SL.RegexSubjectBytes", "subject exceeds byte budget", location);
    }
    return RE2::PartialMatch(subject, expression);
}

size_t Utf8CodePointLengthBounded(std::string_view text, size_t limit) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t width = 1;
        if ((c & 0x80u) == 0u) width = 1;
        else if ((c & 0xE0u) == 0xC0u) width = 2;
        else if ((c & 0xF0u) == 0xE0u) width = 3;
        else if ((c & 0xF8u) == 0xF0u) width = 4;
        if (i + width > text.size()) width = 1;
        i += width;
        ++count;
        if (count > limit) return count;
    }
    return count;
}

bool ExactEqual(const JsonValue& left, const JsonValue& right) {
    if (left.IsNumber() && right.IsNumber()) {
        return ExactNumber::FromJson(left).Compare(ExactNumber::FromJson(right)) == 0;
    }
    if (left.IsArray() && right.IsArray()) {
        if (left.Size() != right.Size()) return false;
        for (size_t i = 0; i < left.Size(); ++i) {
            if (!ExactEqual(left.GetArray()[i], right.GetArray()[i])) return false;
        }
        return true;
    }
    if (left.IsObject() && right.IsObject()) {
        if (left.Size() != right.Size()) return false;
        for (const auto& [key, value] : left.GetObject()) {
            const JsonValue* other = right.Find(key);
            if (!other || !ExactEqual(value, *other)) return false;
        }
        return true;
    }
    return left == right;
}

const std::set<std::string>& SupportedKeywords() {
    static const std::set<std::string> keywords = {
        "$id", "$schema", "$anchor", "$dynamicAnchor", "$dynamicRef", "$recursiveAnchor",
        "$recursiveRef", "$ref", "$comment", "$vocabulary", "$defs", "definitions",
        "type", "const", "enum", "multipleOf", "maximum", "exclusiveMaximum", "minimum",
        "exclusiveMinimum", "maxLength", "minLength", "pattern", "maxItems", "minItems",
        "uniqueItems", "maxContains", "minContains", "maxProperties", "minProperties",
        "required", "dependentRequired", "dependentSchemas", "dependencies", "items",
        "prefixItems", "additionalItems", "contains", "allOf", "anyOf", "oneOf", "not",
        "if", "then", "else", "properties", "patternProperties", "additionalProperties",
        "propertyNames", "unevaluatedProperties", "unevaluatedItems", "format",
        "contentEncoding", "contentMediaType", "contentSchema", "title", "description",
        "default", "examples", "readOnly", "writeOnly", "deprecated"
    };
    return keywords;
}

bool IsSchemaPosition(const std::string& key) {
    static const std::set<std::string> positions = {
        "items", "additionalItems", "additionalProperties", "contains", "not", "if",
        "then", "else", "propertyNames", "unevaluatedProperties", "unevaluatedItems",
        "contentSchema"
    };
    return positions.count(key) > 0;
}

bool IsSchemaContainer(const std::string& key) {
    static const std::set<std::string> containers = {
        "properties", "$defs", "definitions", "dependentSchemas"
    };
    return containers.count(key) > 0;
}

bool IsSupportedDialect(const std::string& dialect) {
    return dialect == "https://json-schema.org/draft/2020-12/schema"
        || dialect == "http://json-schema.org/draft/2020-12/schema";
}

class SchemaRepository {
public:
    SchemaRepository(const SchemaOptions& options, CompiledSchema::Impl& impl)
        : options_(options), impl_(impl) {}

    void Build(const JsonValue& schema) {
        std::string location = "#";
        if (!schema.IsObject() && !schema.IsBool()) {
            SchemaFail("SL.InvalidSchema", "schema must be an object or boolean", location);
        }
        CheckDocumentBytes(schema, location);
        impl_.documents.push_back(schema);
        const JsonValue* root = &impl_.documents.back();
        impl_.root = root;
        std::string root_id = ReadId(*root, location, "");
        impl_.root_id = root_id;
        RegisterResource(root_id, root);
        std::vector<std::pair<const JsonValue*, std::string>> documents{{root, root_id}};
        for (const auto& [uri, document] : options_.registry) {
            if (!document.IsObject() && !document.IsBool()) {
                SchemaFail("SL.InvalidSchema", "registry schema must be an object or boolean", uri);
            }
            CheckDocumentBytes(document, uri);
            impl_.documents.push_back(document);
            const JsonValue* registered = &impl_.documents.back();
            std::string id = ReadId(*registered, uri, "");
            if (!uri.empty()) id = NormalizeUri(uri);
            RegisterResource(id, registered);
            documents.emplace_back(registered, id);
        }
        for (const auto& [document, id] : documents) {
            Traverse(*document, document, id, 0, true, id.empty() ? location : id, true);
        }
        for (const auto& [document, id] : documents) {
            Traverse(*document, document, id, 0, true, id.empty() ? location : id, false);
        }
    }

private:
    void CheckDocumentBytes(const JsonValue& document, const std::string& location) {
        size_t bytes = document.Dump().size();
        total_bytes_ += bytes;
        if (bytes > options_.budgets.max_document_bytes) {
            SchemaFail("SL.SchemaDocBytes", "schema document exceeds byte budget", location);
        }
        if (total_bytes_ > options_.budgets.max_total_bytes) {
            SchemaFail("SL.SchemaTotalBytes", "schema documents exceed total byte budget", location);
        }
        if (++document_count_ > options_.budgets.max_documents) {
            SchemaFail("SL.SchemaDocCount", "schema document count exceeded", location);
        }
    }

    std::string ReadId(const JsonValue& node, const std::string& location,
                       const std::string& inherited) {
        const JsonValue* id = node.IsObject() ? node.Find("$id") : nullptr;
        if (!id) return inherited;
        if (!id->IsString()) SchemaFail("SL.InvalidKeyword", "$id must be a string", location);
        std::string value = NormalizeUri(JoinUri(inherited, id->GetString()));
        if (!HasUriScheme(value)) {
            SchemaFail("SL.RemoteRef", "$id must be an absolute URI", location);
        }
        return value;
    }

    void RegisterResource(const std::string& id, const JsonValue* node) {
        if (id.empty()) return;
        auto inserted = impl_.resources.emplace(id, ResourceEntry{node, id});
        if (!inserted.second && inserted.first->second.root != node) {
            SchemaFail("SL.DuplicateId", "duplicate schema resource identifier", id);
        }
    }

    void RegisterAnchor(const JsonValue* node, const JsonValue* resource,
                        const std::string& resource_id, const std::string& name, bool dynamic,
                        const std::string& location) {
        if (name.empty()) SchemaFail("SL.InvalidKeyword", "empty anchor", location);
        std::string key = resource_id + "#" + name;
        AnchorEntry entry{node, resource, dynamic};
        auto inserted = impl_.anchors.emplace(key, entry);
        if (!inserted.second && inserted.first->second.node != node) {
            SchemaFail("SL.DuplicateAnchor", "duplicate schema anchor", location);
        }
        if (dynamic && resource) {
            impl_.dynamic_anchors[resource][name] = node;
        }
    }

    void Traverse(const JsonValue& node, const JsonValue* resource,
                  const std::string& resource_id, size_t depth, bool is_schema,
                  const std::string& location, bool collect) {
        if (collect && depth > options_.budgets.max_schema_depth) {
            SchemaFail("SL.SchemaDepth", "schema depth exceeded", location);
        }
        if (collect && ++node_count_ > options_.budgets.max_schema_nodes) {
            SchemaFail("SL.SchemaNodes", "schema node count exceeded", location);
        }
        if (node.IsBool()) return;
        if (node.IsArray()) {
            for (size_t i = 0; i < node.Size(); ++i) {
                Traverse(node.GetArray()[i], resource, resource_id, depth + 1, is_schema,
                         location + "/" + std::to_string(i), collect);
            }
            return;
        }
        if (!node.IsObject()) {
            if (is_schema) SchemaFail("SL.InvalidSchema", "schema node must be object or boolean", location);
            return;
        }

        const JsonValue* current_resource = resource;
        std::string current_id = resource_id;
        if (is_schema) {
            if (const JsonValue* id = node.Find("$id")) {
                if (!id->IsString()) SchemaFail("SL.InvalidKeyword", "$id must be a string", location);
                current_id = NormalizeUri(JoinUri(resource_id, id->GetString()));
                current_resource = &node;
                if (collect) RegisterResource(current_id, current_resource);
            }
            if (!collect) {
                for (const char* ref_key : {"$ref", "$dynamicRef", "$recursiveRef"}) {
                    if (const JsonValue* ref = node.Find(ref_key)) {
                        if (!ref->IsString()) SchemaFail("SL.InvalidKeyword", "reference must be a string", location);
                        ResolveReference(current_id, current_resource, ref->GetString(),
                                         location + "/" + ref_key);
                    }
                }
            } else {
                if (const JsonValue* dialect = node.Find("$schema")) {
                    if (!dialect->IsString()) {
                        SchemaFail("SL.UnsupportedDialect", "$schema must be a string", location);
                    }
                    if (!IsSupportedDialect(dialect->GetString())) {
                        SchemaFail("SL.UnsupportedDialect",
                            "schema dialect '" + dialect->GetString()
                            + "' is not supported; custom metaschemas and $vocabulary are rejected at compile time",
                            location);
                    }
                }
                for (const auto& [key, ignored] : node.GetObject()) {
                    (void)ignored;
                    if (!SupportedKeywords().count(key)) {
                        SchemaFail("SL.UnsupportedKeyword", "unsupported keyword '" + key + "'", location);
                    }
                }
                if (const JsonValue* anchor = node.Find("$anchor")) {
                    if (!anchor->IsString()) SchemaFail("SL.InvalidKeyword", "$anchor must be a string", location);
                    RegisterAnchor(&node, current_resource, current_id, anchor->GetString(), false, location);
                }
                if (const JsonValue* anchor = node.Find("$dynamicAnchor")) {
                    if (!anchor->IsString()) SchemaFail("SL.InvalidKeyword", "$dynamicAnchor must be a string", location);
                    RegisterAnchor(&node, current_resource, current_id, anchor->GetString(), true, location);
                }
                if (const JsonValue* pattern = node.Find("pattern")) {
                    if (!pattern->IsString()) SchemaFail("SL.InvalidKeyword", "pattern must be a string", location);
                    CompileRegex(pattern->GetString(), location + "/pattern");
                }
            }
        }

        for (const auto& [key, value] : node.GetObject()) {
            bool child_is_schema = is_schema && IsSchemaPosition(key);
            if (is_schema && IsSchemaContainer(key) && value.IsObject()) {
                for (const auto& [name, sub] : value.GetObject()) {
                    Traverse(sub, current_resource, current_id, depth + 1, true,
                             location + "/" + key + "/" + name, collect);
                }
                continue;
            }
            if (is_schema && key == "patternProperties" && value.IsObject()) {
                for (const auto& [pattern, sub] : value.GetObject()) {
                    if (collect) CompileRegex(pattern, location + "/patternProperties");
                    Traverse(sub, current_resource, current_id, depth + 1, true,
                             location + "/patternProperties/" + pattern, collect);
                }
                continue;
            }
            if (is_schema && key == "allOf" && value.IsArray()) {
                for (size_t i = 0; i < value.Size(); ++i) {
                    Traverse(value.GetArray()[i], current_resource, current_id, depth + 1, true,
                             location + "/allOf/" + std::to_string(i), collect);
                }
                continue;
            }
            if (is_schema && (key == "anyOf" || key == "oneOf" || key == "prefixItems") && value.IsArray()) {
                for (size_t i = 0; i < value.Size(); ++i) {
                    Traverse(value.GetArray()[i], current_resource, current_id, depth + 1, true,
                             location + "/" + key + "/" + std::to_string(i), collect);
                }
                continue;
            }
            Traverse(value, current_resource, current_id, depth + 1, child_is_schema,
                     location + "/" + key, collect);
        }
    }

    void CompileRegex(const std::string& pattern, const std::string& location) {
        if (pattern.size() > options_.budgets.max_pattern_bytes) {
            SchemaFail("SL.RegexPatternBytes", "pattern exceeds byte budget", location);
        }
        const void* key = static_cast<const void*>(pattern.data());
        if (impl_.regexes.count(key)) return;
        if (impl_.regexes.size() >= options_.budgets.max_regex_cache_entries) {
            SchemaFail("SL.RegexCacheEntries", "regular expression cache exhausted", location);
        }
        RE2::Options expression_options;
        expression_options.set_encoding(RE2::Options::EncodingUTF8);
        expression_options.set_max_mem(static_cast<int64_t>(options_.budgets.max_regex_memory));
        expression_options.set_log_errors(false);
        std::string translated = TranslateEcmaPattern(pattern);
        auto expression = std::make_shared<RE2>(translated, expression_options);
        if (!expression->ok()) {
            SchemaFail("SL.RegexFailOpen", "invalid regular expression '" + pattern + "'", location);
        }
        impl_.regexes.emplace(key, RegexHolder{std::move(expression)});
    }

    const JsonValue* ResolveReference(const std::string& base_id, const JsonValue* base_resource,
                                      const std::string& reference, const std::string& location) {
        if (reference.empty()) return base_resource;
        if (reference[0] == '#') return ResolveFragment(base_resource, reference.substr(1), base_id, location);
        size_t hash = reference.find('#');
        std::string uri = reference.substr(0, hash);
        std::string fragment = hash == std::string::npos ? "" : reference.substr(hash + 1);
        std::string resolved = NormalizeUri(JoinUri(base_id, uri));
        auto found = impl_.resources.find(resolved);
        if (found == impl_.resources.end()) {
            SchemaFail("SL.RemoteRef", "remote reference not dereferenced '" + reference + "'", location);
        }
        return ResolveFragment(found->second.root, fragment, resolved, location);
    }

    const JsonValue* ResolveFragment(const JsonValue* resource, const std::string& raw_fragment,
                                     const std::string& resource_id, const std::string& location) {
        if (!resource) SchemaFail("SL.UnresolvedRef", "missing reference resource", location);
        std::string fragment = PercentDecode(raw_fragment);
        if (fragment.empty()) return resource;
        if (fragment[0] == '/') return ResolveJsonPointerLocal(*resource, fragment.substr(1), location);
        std::string key = resource_id + "#" + fragment;
        auto found = impl_.anchors.find(key);
        if (found == impl_.anchors.end()) {
            SchemaFail("SL.UnresolvedRef", "unresolvable anchor '" + fragment + "'", location);
        }
        return found->second.node;
    }

    const SchemaOptions& options_;
    CompiledSchema::Impl& impl_;
    size_t node_count_ = 0;
    size_t total_bytes_ = 0;
    size_t document_count_ = 0;
};

std::string SchemaPath(const std::string& parent, const std::string& key) {
    return parent + "/" + key;
}

std::string InstancePath(const std::string& parent, const std::string& key) {
    return parent + "/" + key;
}

void CheckBudget(EvalContext& ctx, const std::string& location) {
    ++ctx.steps;
    if (ctx.steps > ctx.budgets.max_evaluation_steps) {
        SchemaFail("SL.EvalSteps", "evaluation step budget exceeded", location);
    }
    if (ctx.steps % 256 == 0) {
        auto elapsed = std::chrono::steady_clock::now() - ctx.started;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                >= ctx.budgets.max_evaluation_time) {
            SchemaFail("SL.EvalTime", "evaluation time budget exceeded", location);
        }
    }
}

void CheckExpand(EvalContext& ctx, const std::string& location) {
    ++ctx.expand_nodes;
    if (ctx.expand_nodes > ctx.budgets.max_expand_nodes) {
        SchemaFail("SL.ExpandNodes", "reference expansion budget exceeded", location);
    }
}

const JsonValue* ResolveEvalReference(const std::string& base_id, const JsonValue* base_resource,
                                      const std::string& reference, EvalContext& ctx,
                                      const JsonValue*& out_resource, std::string& out_id,
                                      const std::string& location) {
    out_resource = base_resource;
    out_id = base_id;
    if (reference.empty()) return base_resource;
    if (reference[0] == '#') {
        std::string fragment = PercentDecode(reference.substr(1));
        if (fragment.empty()) return base_resource;
        if (fragment[0] == '/') {
            return ResolveJsonPointerLocal(*base_resource, fragment.substr(1), location);
        }
        std::string key = base_id + "#" + fragment;
        auto found = ctx.anchors.find(key);
        if (found == ctx.anchors.end()) {
            SchemaFail("SL.UnresolvedRef", "unresolvable anchor '" + fragment + "'", location);
        }
        out_resource = found->second.resource;
        out_id = key.substr(0, key.find('#'));
        return found->second.node;
    }
    size_t hash = reference.find('#');
    std::string uri = reference.substr(0, hash);
    std::string fragment = hash == std::string::npos ? ""
        : PercentDecode(reference.substr(hash + 1));
    std::string resolved = NormalizeUri(JoinUri(base_id, uri));
    auto resource_found = ctx.resources.find(resolved);
    if (resource_found == ctx.resources.end()) {
        SchemaFail("SL.RemoteRef", "remote reference not dereferenced '" + reference + "'", location);
    }
    const JsonValue* target = resource_found->second.root;
    out_resource = target;
    out_id = resolved;
    if (fragment.empty()) return target;
    if (fragment[0] == '/') return ResolveJsonPointerLocal(*target, fragment.substr(1), location);
    auto anchor_found = ctx.anchors.find(resolved + "#" + fragment);
    if (anchor_found == ctx.anchors.end()) {
        SchemaFail("SL.UnresolvedRef", "unresolvable anchor '" + fragment + "'", location);
    }
    out_resource = anchor_found->second.resource;
    return anchor_found->second.node;
}

bool IsTypeMatch(const JsonValue& instance, const std::string& type_name) {
    if (type_name == "integer") {
        if (instance.IsInt()) return true;
        if (instance.IsDouble()) return ExactNumber::FromJson(instance).IsInteger();
        return false;
    }
    if (type_name == "object") return instance.IsObject();
    if (type_name == "array") return instance.IsArray();
    if (type_name == "string") return instance.IsString();
    if (type_name == "number") return instance.IsNumber();
    if (type_name == "boolean") return instance.IsBool();
    if (type_name == "null") return instance.IsNull();
    SchemaFail("SL.InvalidKeyword", "unknown type name '" + type_name + "'", "type");
}

bool FormatMatches(const std::string& format, const std::string& value,
                   EvalContext& ctx, const std::string& location) {
    if (format == "regex") {
        if (value.size() > ctx.budgets.max_pattern_bytes) {
            SchemaFail("SL.RegexPatternBytes", "regex format value exceeds budget", location);
        }
        RE2::Options options;
        options.set_encoding(RE2::Options::EncodingUTF8);
        options.set_max_mem(static_cast<int64_t>(ctx.budgets.max_regex_memory));
        options.set_log_errors(false);
        RE2 expression(TranslateEcmaPattern(value), options);
        return expression.ok();
    }
    if (format == "date-time") {
        return value.size() >= 20 && value[4] == '-' && value[7] == '-'
            && (value[10] == 'T' || value[10] == 't') && value[13] == ':'
            && value[16] == ':' && (value.back() == 'Z' || value.back() == 'z');
    }
    if (format == "date") return value.size() == 10 && value[4] == '-' && value[7] == '-';
    if (format == "time") return value.size() >= 8 && value[2] == ':' && value[5] == ':';
    if (format == "ipv4") {
        static const RE2 expression(
            R"(^(?:(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])$)");
        return RE2::FullMatch(value, expression);
    }
    if (format == "email") {
        static const RE2 expression(R"(^[^@\s]+@[^@\s]+$)");
        return RE2::FullMatch(value, expression);
    }
    return true;
}

EvalResult Evaluate(const JsonValue& instance, const JsonValue& schema,
                    const JsonValue* resource, std::string resource_id,
                    const std::string& instance_path, const std::string& schema_path,
                    EvalContext& ctx, std::string& error_out);

EvalResult EvaluateReference(const JsonValue& instance, const JsonValue& target,
                             const JsonValue* target_resource, std::string target_id,
                             const std::string& instance_path, const std::string& schema_path,
                             EvalContext& ctx, std::string& error_out) {
    CheckExpand(ctx, schema_path);
    if (ctx.reference_depth >= ctx.budgets.max_reference_depth) {
        SchemaFail("SL.RefDepth", "reference depth budget exceeded", schema_path);
    }
    auto active_key = std::make_pair(static_cast<const void*>(&target), instance_path);
    if (ctx.active.count(active_key)) {
        EvalResult result;
        result.valid = true;
        return result;
    }
    ctx.active.insert(active_key);
    ++ctx.reference_depth;
    EvalResult result;
    try {
        result = Evaluate(instance, target, target_resource, target_id, instance_path,
                          schema_path, ctx, error_out);
    } catch (...) {
        --ctx.reference_depth;
        ctx.active.erase(active_key);
        throw;
    }
    --ctx.reference_depth;
    ctx.active.erase(active_key);
    return result;
}

void EvaluateCombination(const JsonValue& instance, const JsonValue& schema,
                         const JsonValue* resource, const std::string& resource_id,
                         const std::string& instance_path, const std::string& schema_path,
                         EvalContext& ctx, std::string& error_out, EvalResult& result) {
    auto fail = [&](const char* classifier, const std::string& message) {
        error_out = std::string(classifier) + ": " + message + " at " + schema_path;
        result.valid = false;
    };
    if (const JsonValue* all_of = schema.Find("allOf")) {
        if (!all_of->IsArray()) SchemaFail("SL.InvalidKeyword", "allOf must be an array",
                                            SchemaPath(schema_path, "allOf"));
        for (size_t i = 0; i < all_of->Size(); ++i) {
            EvalResult child = Evaluate(instance, all_of->GetArray()[i], resource, resource_id,
                                        instance_path,
                                        SchemaPath(SchemaPath(schema_path, "allOf"), std::to_string(i)),
                                        ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
            MergeResult(result, child);
        }
    }
    if (const JsonValue* any_of = schema.Find("anyOf")) {
        if (!any_of->IsArray()) SchemaFail("SL.InvalidKeyword", "anyOf must be an array",
                                            SchemaPath(schema_path, "anyOf"));
        bool matched = false;
        std::string ignored;
        for (size_t i = 0; i < any_of->Size(); ++i) {
            EvalResult child = Evaluate(instance, any_of->GetArray()[i], resource, resource_id,
                                        instance_path,
                                        SchemaPath(SchemaPath(schema_path, "anyOf"), std::to_string(i)),
                                        ctx, ignored);
            if (child.valid) {
                matched = true;
                MergeResult(result, child);
            }
        }
        if (!matched) {
            fail("SL.AnyOf", "value does not match anyOf");
            return;
        }
    }
    if (const JsonValue* one_of = schema.Find("oneOf")) {
        if (!one_of->IsArray()) SchemaFail("SL.InvalidKeyword", "oneOf must be an array",
                                            SchemaPath(schema_path, "oneOf"));
        size_t matched = 0;
        EvalResult matched_result;
        std::string ignored;
        for (size_t i = 0; i < one_of->Size(); ++i) {
            EvalResult child = Evaluate(instance, one_of->GetArray()[i], resource, resource_id,
                                        instance_path,
                                        SchemaPath(SchemaPath(schema_path, "oneOf"), std::to_string(i)),
                                        ctx, ignored);
            if (child.valid) {
                ++matched;
                matched_result = child;
            }
        }
        if (matched != 1) {
            fail("SL.OneOf", "value matches oneOf branches other than exactly one");
            return;
        }
        MergeResult(result, matched_result);
    }
    if (const JsonValue* forbidden = schema.Find("not")) {
        std::string ignored;
        EvalResult child = Evaluate(instance, *forbidden, resource, resource_id,
                                    instance_path, SchemaPath(schema_path, "not"), ctx, ignored);
        if (child.valid) {
            fail("SL.Not", "value matches forbidden schema");
            return;
        }
    }
    if (const JsonValue* condition = schema.Find("if")) {
        std::string ignored;
        EvalResult child = Evaluate(instance, *condition, resource, resource_id,
                                    instance_path, SchemaPath(schema_path, "if"), ctx, ignored);
        if (child.valid) MergeResult(result, child);
        const JsonValue* branch = child.valid ? schema.Find("then") : schema.Find("else");
        if (branch) {
            EvalResult applied = Evaluate(instance, *branch, resource, resource_id,
                                          instance_path,
                                          SchemaPath(schema_path, child.valid ? "then" : "else"),
                                          ctx, error_out);
            if (!applied.valid) {
                result.valid = false;
                return;
            }
            MergeResult(result, applied);
        }
    }
}

void EvaluateObject(const JsonValue& instance, const JsonValue& schema,
                    const JsonValue* resource, const std::string& resource_id,
                    const std::string& instance_path, const std::string& schema_path,
                    EvalContext& ctx, std::string& error_out, EvalResult& result) {
    const auto& object = instance.GetObject();
    auto fail = [&](const char* classifier, const std::string& message) {
        error_out = std::string(classifier) + ": " + message + " at " + schema_path;
        result.valid = false;
    };
    if (const JsonValue* required = schema.Find("required")) {
        if (!required->IsArray()) SchemaFail("SL.InvalidKeyword", "required must be an array",
                                              SchemaPath(schema_path, "required"));
        for (const auto& name : required->GetArray()) {
            if (!name.IsString()) SchemaFail("SL.InvalidKeyword", "required entry must be a string",
                                              SchemaPath(schema_path, "required"));
            if (!instance.Contains(name.GetString())) {
                fail("SL.Required", "missing required property '" + name.GetString() + "'");
                return;
            }
        }
    }
    if (const JsonValue* minimum = schema.Find("minProperties")) {
        size_t bound = ReadCount(*minimum, SchemaPath(schema_path, "minProperties"), ctx.budgets.max_schema_nodes);
        if (object.size() < bound) {
            fail("SL.MinProperties", "too few properties");
            return;
        }
    }
    if (const JsonValue* maximum = schema.Find("maxProperties")) {
        size_t bound = ReadCount(*maximum, SchemaPath(schema_path, "maxProperties"), ctx.budgets.max_schema_nodes);
        if (object.size() > bound) {
            fail("SL.MaxProperties", "too many properties");
            return;
        }
    }
    const JsonValue* properties = schema.Find("properties");
    if (properties && !properties->IsObject()) SchemaFail("SL.InvalidKeyword", "properties must be an object",
                                                           SchemaPath(schema_path, "properties"));
    const JsonValue* patterns = schema.Find("patternProperties");
    if (patterns && !patterns->IsObject()) SchemaFail("SL.InvalidKeyword", "patternProperties must be an object",
                                                       SchemaPath(schema_path, "patternProperties"));
    std::set<std::string> covered;
    if (properties) {
        for (const auto& [key, sub] : properties->GetObject()) {
            const JsonValue* value = instance.Find(key);
            if (!value) continue;
            EvalResult child = Evaluate(*value, sub, resource, resource_id,
                                        InstancePath(instance_path, "properties/" + key),
                                        SchemaPath(SchemaPath(schema_path, "properties"), key),
                                        ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
            covered.insert(key);
            result.properties.insert(key);
        }
    }
    if (patterns) {
        for (const auto& [pattern, sub] : patterns->GetObject()) {
            const RE2& expression = GetRegex(pattern, ctx,
                                             SchemaPath(schema_path, "patternProperties"));
            for (const auto& [key, value] : object) {
                if (!RegexMatches(expression, key, ctx,
                                  SchemaPath(schema_path, "patternProperties"))) continue;
                EvalResult child = Evaluate(value, sub, resource, resource_id,
                                            InstancePath(instance_path, "properties/" + key),
                                            SchemaPath(SchemaPath(schema_path, "patternProperties"), pattern),
                                            ctx, error_out);
                if (!child.valid) {
                    result.valid = false;
                    return;
                }
                covered.insert(key);
                result.properties.insert(key);
            }
        }
    }
    if (const JsonValue* additional = schema.Find("additionalProperties")) {
        for (const auto& [key, value] : object) {
            if (covered.count(key)) continue;
            EvalResult child = Evaluate(value, *additional, resource, resource_id,
                                        InstancePath(instance_path, "properties/" + key),
                                        SchemaPath(schema_path, "additionalProperties"),
                                        ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
            result.properties.insert(key);
        }
    }
    if (const JsonValue* names = schema.Find("propertyNames")) {
        for (const auto& [key, ignored] : object) {
            (void)ignored;
            JsonValue key_value(key);
            EvalResult child = Evaluate(key_value, *names, resource, resource_id,
                                        InstancePath(instance_path, "propertyNames"),
                                        SchemaPath(schema_path, "propertyNames"),
                                        ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
        }
    }
    if (const JsonValue* required_by = schema.Find("dependentRequired")) {
        if (!required_by->IsObject()) SchemaFail("SL.InvalidKeyword", "dependentRequired must be an object",
                                                  SchemaPath(schema_path, "dependentRequired"));
        for (const auto& [key, dependencies] : required_by->GetObject()) {
            if (!instance.Contains(key) || !dependencies.IsArray()) continue;
            for (const auto& dependency : dependencies.GetArray()) {
                if (!dependency.IsString()) SchemaFail("SL.InvalidKeyword", "dependency must be a string",
                                                        SchemaPath(schema_path, "dependentRequired"));
                if (!instance.Contains(dependency.GetString())) {
                    fail("SL.DependentRequired", "missing dependent property '" + dependency.GetString() + "'");
                    return;
                }
            }
        }
    }
    if (const JsonValue* schemas_by = schema.Find("dependentSchemas")) {
        if (!schemas_by->IsObject()) SchemaFail("SL.InvalidKeyword", "dependentSchemas must be an object",
                                                 SchemaPath(schema_path, "dependentSchemas"));
        for (const auto& [key, sub] : schemas_by->GetObject()) {
            if (!instance.Contains(key)) continue;
            EvalResult child = Evaluate(instance, sub, resource, resource_id,
                                        InstancePath(instance_path, "dependentSchemas/" + key),
                                        SchemaPath(SchemaPath(schema_path, "dependentSchemas"), key),
                                        ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
            MergeResult(result, child);
        }
    }
    if (const JsonValue* dependencies = schema.Find("dependencies")) {
        if (!dependencies->IsObject()) SchemaFail("SL.InvalidKeyword", "dependencies must be an object",
                                                   SchemaPath(schema_path, "dependencies"));
        for (const auto& [key, sub] : dependencies->GetObject()) {
            if (!instance.Contains(key)) continue;
            if (sub.IsArray()) {
                for (const auto& dependency : sub.GetArray()) {
                    if (!dependency.IsString()) SchemaFail("SL.InvalidKeyword", "dependency must be a string",
                                                            SchemaPath(schema_path, "dependencies"));
                    if (!instance.Contains(dependency.GetString())) {
                        fail("SL.DependentRequired", "missing dependent property '" + dependency.GetString() + "'");
                        return;
                    }
                }
            } else {
                EvalResult child = Evaluate(instance, sub, resource, resource_id,
                                            InstancePath(instance_path, "dependencies/" + key),
                                            SchemaPath(SchemaPath(schema_path, "dependencies"), key),
                                            ctx, error_out);
                if (!child.valid) {
                    result.valid = false;
                    return;
                }
                MergeResult(result, child);
            }
        }
    }
}

void EvaluateArray(const JsonValue& instance, const JsonValue& schema,
                   const JsonValue* resource, const std::string& resource_id,
                   const std::string& instance_path, const std::string& schema_path,
                   EvalContext& ctx, std::string& error_out, EvalResult& result) {
    const auto& array = instance.GetArray();
    auto fail = [&](const char* classifier, const std::string& message) {
        error_out = std::string(classifier) + ": " + message + " at " + schema_path;
        result.valid = false;
    };
    const JsonValue* prefix = schema.Find("prefixItems");
    if (prefix && !prefix->IsArray()) SchemaFail("SL.InvalidKeyword", "prefixItems must be an array",
                                                  SchemaPath(schema_path, "prefixItems"));
    size_t prefix_count = prefix ? prefix->Size() : 0;
    const JsonValue* items = schema.Find("items");
    if (items && items->IsArray()) SchemaFail("SL.InvalidKeyword", "items must not be an array in 2020-12",
                                               SchemaPath(schema_path, "items"));
    for (size_t i = 0; i < array.size(); ++i) {
        const JsonValue* sub = nullptr;
        std::string keyword;
        if (prefix && i < prefix_count) {
            sub = &prefix->GetArray()[i];
            keyword = "prefixItems/" + std::to_string(i);
        } else if (items) {
            sub = items;
            keyword = "items";
        }
        if (!sub) continue;
        EvalResult child = Evaluate(array[i], *sub, resource, resource_id,
                                    InstancePath(instance_path, "items/" + std::to_string(i)),
                                    SchemaPath(schema_path, keyword), ctx, error_out);
        if (!child.valid) {
            result.valid = false;
            return;
        }
        result.items.insert(i);
    }
    if (const JsonValue* additional = schema.Find("additionalItems")) {
        for (size_t i = prefix_count; i < array.size(); ++i) {
            EvalResult child = Evaluate(array[i], *additional, resource, resource_id,
                                        InstancePath(instance_path, "items/" + std::to_string(i)),
                                        SchemaPath(schema_path, "additionalItems"), ctx, error_out);
            if (!child.valid) {
                result.valid = false;
                return;
            }
            result.items.insert(i);
        }
    }
    size_t minimum = 1;
    size_t maximum = array.size() + 1;
    if (const JsonValue* contains = schema.Find("contains")) {
        size_t matched = 0;
        for (size_t i = 0; i < array.size(); ++i) {
            EvalResult child = Evaluate(array[i], *contains, resource, resource_id,
                                        InstancePath(instance_path, "items/" + std::to_string(i)),
                                        SchemaPath(schema_path, "contains"), ctx, error_out);
            if (child.valid) {
                ++matched;
                result.items.insert(i);
            }
        }
        if (const JsonValue* min_contains = schema.Find("minContains")) {
            minimum = ReadCount(*min_contains, SchemaPath(schema_path, "minContains"), array.size() + 1);
        }
        if (const JsonValue* max_contains = schema.Find("maxContains")) {
            maximum = ReadCount(*max_contains, SchemaPath(schema_path, "maxContains"), array.size() + 1);
        }
        if (matched < minimum || matched > maximum) {
            fail("SL.Contains", "array contains outside min/maxContains");
            return;
        }
    }
    if (const JsonValue* unique = schema.Find("uniqueItems")) {
        if (ReadBoolean(*unique, SchemaPath(schema_path, "uniqueItems"))) {
            if (array.size() > ctx.budgets.max_unique_items) {
                SchemaFail("SL.UniqueItemsSize", "uniqueItems element budget exceeded",
                           SchemaPath(schema_path, "uniqueItems"));
            }
            for (size_t i = 0; i < array.size(); ++i) {
                for (size_t j = i + 1; j < array.size(); ++j) {
                    if (ExactEqual(array[i], array[j])) {
                        fail("SL.UniqueItems", "array items not unique");
                        return;
                    }
                }
            }
        }
    }
    if (const JsonValue* minimum_items = schema.Find("minItems")) {
        size_t bound = ReadCount(*minimum_items, SchemaPath(schema_path, "minItems"), ctx.budgets.max_expand_nodes);
        if (array.size() < bound) {
            fail("SL.MinItems", "too few items");
            return;
        }
    }
    if (const JsonValue* maximum_items = schema.Find("maxItems")) {
        size_t bound = ReadCount(*maximum_items, SchemaPath(schema_path, "maxItems"), ctx.budgets.max_expand_nodes);
        if (array.size() > bound) {
            fail("SL.MaxItems", "too many items");
            return;
        }
    }
}

void EvaluateUnevaluated(const JsonValue& instance, const JsonValue& schema,
                         const JsonValue* resource, const std::string& resource_id,
                         const std::string& instance_path, const std::string& schema_path,
                         EvalContext& ctx, std::string& error_out, EvalResult& result) {
    if (instance.IsObject()) {
        if (const JsonValue* unevaluated = schema.Find("unevaluatedProperties")) {
            for (const auto& [key, value] : instance.GetObject()) {
                if (result.properties.count(key)) continue;
                EvalResult child = Evaluate(value, *unevaluated, resource, resource_id,
                                            InstancePath(instance_path, "properties/" + key),
                                            SchemaPath(schema_path, "unevaluatedProperties"),
                                            ctx, error_out);
                if (!child.valid) {
                    result.valid = false;
                    return;
                }
                result.properties.insert(key);
            }
        }
    }
    if (instance.IsArray()) {
        if (const JsonValue* unevaluated = schema.Find("unevaluatedItems")) {
            for (size_t i = 0; i < instance.Size(); ++i) {
                if (result.items.count(i)) continue;
                EvalResult child = Evaluate(instance.GetArray()[i], *unevaluated, resource, resource_id,
                                            InstancePath(instance_path, "items/" + std::to_string(i)),
                                            SchemaPath(schema_path, "unevaluatedItems"), ctx, error_out);
                if (!child.valid) {
                    result.valid = false;
                    return;
                }
                result.items.insert(i);
            }
        }
    }
}

EvalResult Evaluate(const JsonValue& instance, const JsonValue& schema,
                    const JsonValue* resource, std::string resource_id,
                    const std::string& instance_path, const std::string& schema_path,
                    EvalContext& ctx, std::string& error_out) {
    CheckBudget(ctx, schema_path);
    const JsonValue* parent_resource = ctx.dynamic_scope.empty()
        ? nullptr : ctx.dynamic_scope.back().resource;
    UpdateResourceContext(schema, resource, resource_id);
    DynamicScopeGuard scope_guard(ctx, resource, resource_id, resource != parent_resource);
    EvalResult result;
    if (schema.IsBool()) {
        if (!schema.GetBool()) {
            error_out = "SL.BooleanSchema: boolean schema false at " + schema_path;
            result.valid = false;
        }
        return result;
    }
    if (!schema.IsObject()) {
        SchemaFail("SL.InvalidSchema", "schema node must be object or boolean", schema_path);
    }
    auto fail = [&](const char* classifier, const std::string& message) {
        error_out = std::string(classifier) + ": " + message + " at " + schema_path;
        result.valid = false;
    };

    if (const JsonValue* ref = schema.Find("$ref")) {
        const JsonValue* target_resource = nullptr;
        std::string target_id;
        const JsonValue* target = ResolveEvalReference(resource_id, resource, ref->GetString(), ctx,
                                                        target_resource, target_id,
                                                        SchemaPath(schema_path, "$ref"));
        EvalResult child = EvaluateReference(instance, *target, target_resource, target_id,
                                             instance_path, SchemaPath(schema_path, "$ref"),
                                             ctx, error_out);
        if (!child.valid) {
            result.valid = false;
            return result;
        }
        MergeResult(result, child);
    }

    if (const JsonValue* dynamic_ref = schema.Find("$dynamicRef")) {
        const std::string& reference = dynamic_ref->GetString();
        size_t hash = reference.find('#');
        std::string anchor = hash == std::string::npos ? "" : PercentDecode(reference.substr(hash + 1));
        const JsonValue* target_resource = nullptr;
        std::string target_id;
        const JsonValue* target = ResolveEvalReference(resource_id, resource, reference, ctx,
                                                        target_resource, target_id,
                                                        SchemaPath(schema_path, "$dynamicRef"));
        bool bookend = !anchor.empty() && anchor[0] != '/' && target->IsObject();
        if (bookend) {
            const JsonValue* declared = target->Find("$dynamicAnchor");
            bookend = declared && declared->IsString() && declared->GetString() == anchor;
        }
        if (bookend) {
            for (const auto& frame : ctx.dynamic_scope) {
                auto found = ctx.dynamic_anchors.find(frame.resource);
                if (found == ctx.dynamic_anchors.end()) continue;
                auto anchor_found = found->second.find(anchor);
                if (anchor_found == found->second.end()) continue;
                target = anchor_found->second;
                target_resource = frame.resource;
                target_id = frame.resource_id;
                break;
            }
        }
        EvalResult child = EvaluateReference(instance, *target, target_resource, target_id,
                                             instance_path, SchemaPath(schema_path, "$dynamicRef"),
                                             ctx, error_out);
        if (!child.valid) {
            result.valid = false;
            return result;
        }
        MergeResult(result, child);
    }

    if (schema.Find("$recursiveRef")) {
        const JsonValue* target = resource;
        for (auto frame = ctx.dynamic_scope.rbegin(); frame != ctx.dynamic_scope.rend(); ++frame) {
            auto found = ctx.dynamic_anchors.find(frame->resource);
            if (found != ctx.dynamic_anchors.end() && found->second.count("#")) {
                target = found->second.at("#");
                break;
            }
        }
        EvalResult child = EvaluateReference(instance, *target, resource, resource_id,
                                             instance_path, SchemaPath(schema_path, "$recursiveRef"),
                                             ctx, error_out);
        if (!child.valid) {
            result.valid = false;
            return result;
        }
        MergeResult(result, child);
    }

    if (const JsonValue* type = schema.Find("type")) {
        bool matched = false;
        if (type->IsString()) matched = IsTypeMatch(instance, type->GetString());
        else if (type->IsArray()) {
            for (const auto& option : type->GetArray()) {
                if (option.IsString() && IsTypeMatch(instance, option.GetString())) {
                    matched = true;
                    break;
                }
            }
        } else {
            SchemaFail("SL.InvalidKeyword", "type must be a string or array", SchemaPath(schema_path, "type"));
        }
        if (!matched) {
            fail("SL.Type", "type mismatch");
            return result;
        }
    }

    if (const JsonValue* constant = schema.Find("const")) {
        if (!ExactEqual(*constant, instance)) {
            fail("SL.Const", "value does not match const");
            return result;
        }
    }

    if (const JsonValue* enumeration = schema.Find("enum")) {
        if (!enumeration->IsArray()) SchemaFail("SL.InvalidKeyword", "enum must be an array",
                                                 SchemaPath(schema_path, "enum"));
        bool matched = false;
        for (const auto& option : enumeration->GetArray()) {
            if (ExactEqual(option, instance)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            fail("SL.Enum", "value not in enum");
            return result;
        }
    }

    if (instance.IsNumber()) {
        ExactNumber value = ExactNumber::FromJson(instance);
        if (const JsonValue* minimum = schema.Find("minimum")) {
            if (value.Compare(ReadNumber(*minimum, SchemaPath(schema_path, "minimum"))) < 0) {
                fail("SL.Minimum", "value below minimum");
                return result;
            }
        }
        if (const JsonValue* maximum = schema.Find("maximum")) {
            if (value.Compare(ReadNumber(*maximum, SchemaPath(schema_path, "maximum"))) > 0) {
                fail("SL.Maximum", "value above maximum");
                return result;
            }
        }
        if (const JsonValue* exclusive = schema.Find("exclusiveMinimum")) {
            if (!exclusive->IsNumber()) SchemaFail("SL.InvalidKeyword", "exclusiveMinimum must be numeric in 2020-12",
                                                    SchemaPath(schema_path, "exclusiveMinimum"));
            if (value.Compare(ReadNumber(*exclusive, SchemaPath(schema_path, "exclusiveMinimum"))) <= 0) {
                fail("SL.ExclusiveMinimum", "value below exclusiveMinimum");
                return result;
            }
        }
        if (const JsonValue* exclusive = schema.Find("exclusiveMaximum")) {
            if (!exclusive->IsNumber()) SchemaFail("SL.InvalidKeyword", "exclusiveMaximum must be numeric in 2020-12",
                                                    SchemaPath(schema_path, "exclusiveMaximum"));
            if (value.Compare(ReadNumber(*exclusive, SchemaPath(schema_path, "exclusiveMaximum"))) >= 0) {
                fail("SL.ExclusiveMaximum", "value above exclusiveMaximum");
                return result;
            }
        }
        if (const JsonValue* multiple = schema.Find("multipleOf")) {
            ExactNumber divisor = ReadNumber(*multiple, SchemaPath(schema_path, "multipleOf"));
            if (!divisor.IsPositive() || !value.IsMultipleOf(divisor)) {
                fail("SL.MultipleOf", "value is not a multiple");
                return result;
            }
        }
    }

    if (instance.IsString()) {
        const std::string& text = instance.GetString();
        size_t length = Utf8CodePointLengthBounded(text, ctx.budgets.max_subject_bytes);
        if (const JsonValue* minimum = schema.Find("minLength")) {
            size_t bound = ReadCount(*minimum, SchemaPath(schema_path, "minLength"), ctx.budgets.max_subject_bytes);
            if (length < bound) {
                fail("SL.MinLength", "string too short");
                return result;
            }
        }
        if (const JsonValue* maximum = schema.Find("maxLength")) {
            size_t bound = ReadCount(*maximum, SchemaPath(schema_path, "maxLength"), ctx.budgets.max_subject_bytes);
            if (length > bound) {
                fail("SL.MaxLength", "string too long");
                return result;
            }
        }
        if (const JsonValue* pattern = schema.Find("pattern")) {
            const RE2& expression = GetRegex(pattern->GetString(), ctx,
                                             SchemaPath(schema_path, "pattern"));
            if (!RegexMatches(expression, text, ctx, SchemaPath(schema_path, "pattern"))) {
                fail("SL.Pattern", "string does not match pattern");
                return result;
            }
        }
        if (const JsonValue* format = schema.Find("format")) {
            if (ctx.assert_format && format->IsString()
                && !FormatMatches(format->GetString(), text, ctx,
                                  SchemaPath(schema_path, "format"))) {
                fail("SL.Format", "string does not match asserted format");
                return result;
            }
        }
    }

    if (instance.IsObject()) EvaluateObject(instance, schema, resource, resource_id,
                                             instance_path, schema_path, ctx, error_out, result);
    if (!result.valid) return result;
    if (instance.IsArray()) EvaluateArray(instance, schema, resource, resource_id,
                                           instance_path, schema_path, ctx, error_out, result);
    if (!result.valid) return result;
    EvaluateCombination(instance, schema, resource, resource_id, instance_path, schema_path,
                        ctx, error_out, result);
    if (!result.valid) return result;
    EvaluateUnevaluated(instance, schema, resource, resource_id, instance_path, schema_path,
                        ctx, error_out, result);
    return result;
}

} // namespace schema

CompiledSchema::CompiledSchema() : impl_(std::make_unique<Impl>()) {}
CompiledSchema::~CompiledSchema() = default;
CompiledSchema::CompiledSchema(CompiledSchema&&) noexcept = default;
CompiledSchema& CompiledSchema::operator=(CompiledSchema&&) noexcept = default;

bool CompiledSchema::Valid() const {
    return impl_ && impl_->ready;
}

CompiledSchema SchemaCompiler::Compile(const JsonValue& schema, const SchemaOptions& options) {
    CompiledSchema compiled;
    schema::SchemaRepository repository(options, *compiled.impl_);
    repository.Build(schema);
    compiled.impl_->options = options;
    compiled.impl_->ready = true;
    return compiled;
}

bool EvaluationEngine::Validate(const CompiledSchema& compiled, const JsonValue& instance,
                                std::string& error_out) {
    if (!compiled.Valid()) {
        error_out = "SL.InvalidSchema: no compiled schema";
        return false;
    }
    try {
        schema::EvalContext context(*compiled.impl_);
        context.dynamic_scope.push_back(schema::DynamicFrame{
            compiled.impl_->root, compiled.impl_->root_id});
        std::string ignored;
        schema::EvalResult result = schema::Evaluate(instance, *compiled.impl_->root,
            compiled.impl_->root, compiled.impl_->root_id, "", "", context, ignored);
        if (!result.valid) error_out = ignored.empty() ? "SL.Validation: schema validation failed" : ignored;
        return result.valid;
    } catch (const McpError& error) {
        error_out = error.what();
        return false;
    } catch (const std::exception& error) {
        error_out = std::string("SL.InternalError: ") + error.what();
        return false;
    }
}

bool ValidateJsonSchema2020(const JsonValue& instance, const JsonValue& schema,
                            std::string& error_out, const SchemaOptions& options) {
    try {
        CompiledSchema compiled = SchemaCompiler::Compile(schema, options);
        return EvaluationEngine::Validate(compiled, instance, error_out);
    } catch (const McpError& error) {
        error_out = error.what();
        return false;
    } catch (const std::exception& error) {
        error_out = std::string("SL.InternalError: ") + error.what();
        return false;
    }
}

bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                        std::string& error_out, SchemaMode mode,
                        const SchemaOptions& options) {
    if (mode == SchemaMode::Modern2020) {
        return ValidateJsonSchema2020(instance, schema, error_out, options);
    }
    return ValidateJsonSchema(instance, schema, error_out);
}

SchemaShadowResult CompareJsonSchemaEngines(const JsonValue& instance,
                                            const JsonValue& schema,
                                            const SchemaOptions& options) {
    SchemaShadowResult result;
    std::string ignored;
    result.legacy_valid = ValidateJsonSchema(instance, schema, ignored, SchemaMode::Legacy, options);
    result.modern_valid = ValidateJsonSchema(instance, schema, ignored, SchemaMode::Modern2020, options);
    result.diverged = result.legacy_valid != result.modern_valid;
    return result;
}

}}
