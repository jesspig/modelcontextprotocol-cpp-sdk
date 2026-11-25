#include <detail/JsonSchemaValidator.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using mcp::JsonValue;

#ifdef MCP_JSON_SCHEMA_SUITE_DIR
std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool HasTag(const JsonValue& test_case, const std::string& tag) {
    const JsonValue* tags = test_case.Find("tags");
    if (!tags || !tags->IsArray()) return false;
    for (const auto& value : tags->GetArray()) {
        if (value.IsString() && value.GetString() == tag) return true;
    }
    return false;
}

bool IsCompatible(const JsonValue& test_case) {
    const JsonValue* compatibility = test_case.Find("compatibility");
    if (!compatibility || !compatibility->IsString()) return true;
    constexpr int version = 2020;
    std::string text = compatibility->GetString();
    size_t start = 0;
    int lower = 0;
    int upper = 2147483647;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        std::string term = text.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        std::string op;
        if (!term.empty() && (term[0] == '<' || term[0] == '=' || term[0] == '>')) {
            if (term.size() > 1 && term[1] == '=') {
                op = term.substr(0, 2);
                term = term.substr(2);
            } else {
                op = term.substr(0, 1);
                term = term.substr(1);
            }
        }
        int bound = -1;
        for (char c : term) {
            if (c < '0' || c > '9') { bound = -1; break; }
            if (bound < 0) bound = 0;
            bound = bound * 10 + (c - '0');
        }
        if (bound >= 0) {
            if (op.empty() || op == ">=") lower = lower > bound ? lower : bound;
            else if (op == ">") lower = lower > bound + 1 ? lower : bound + 1;
            else if (op == "<=") upper = upper < bound ? upper : bound;
            else if (op == "<") upper = upper < bound - 1 ? upper : bound - 1;
            else if (op == "=") {
                lower = lower > bound ? lower : bound;
                upper = upper < bound ? upper : bound;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return version >= lower && version <= upper;
}
#endif

bool ModernValid(const JsonValue& instance, const JsonValue& schema, std::string& error,
                 const mcp::detail::SchemaOptions& options = {}) {
    return mcp::detail::ValidateJsonSchema2020(instance, schema, error, options);
}

TEST(JsonSchema2020Test, ExactIntegersBeyondDoublePrecision) {
    std::string error;
    auto schema = JsonValue::Parse(R"({"const":9007199254740993})");
    EXPECT_TRUE(ModernValid(JsonValue::Parse("9007199254740993"), schema, error));
    EXPECT_FALSE(ModernValid(JsonValue::Parse("9007199254740992"), schema, error));
    auto minimum = JsonValue::Parse(R"({"minimum":9007199254740993})");
    EXPECT_FALSE(ModernValid(JsonValue::Parse("9007199254740992"), minimum, error));
    EXPECT_TRUE(ModernValid(JsonValue::Parse("9007199254740993"), minimum, error));
}

TEST(JsonSchema2020Test, ExactDecimalMultipleOf) {
    std::string error;
    auto schema = JsonValue::Parse(R"({"multipleOf":0.1})");
    EXPECT_TRUE(ModernValid(JsonValue::Parse("0.3"), schema, error));
    EXPECT_FALSE(ModernValid(JsonValue::Parse("0.31"), schema, error));
    auto negative_zero = JsonValue::Parse(R"({"const":0})");
    EXPECT_TRUE(ModernValid(JsonValue::Parse("-0.0"), negative_zero, error));
}

TEST(JsonSchema2020Test, DynamicRefUsesDynamicAnchor) {
    std::string error;
    auto valid_schema = JsonValue::Parse(R"({
        "$id":"https://example.com/tree.json",
        "$dynamicAnchor":"node",
        "type":"object",
        "properties":{
            "data":{"type":"string"},
            "child":{"$dynamicRef":"#node"}
        }
    })");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"data":"x","child":{"data":"y"}})"),
                            valid_schema, error)) << error;
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"({"data":"x","child":{"data":7}})"),
                             valid_schema, error));
}

TEST(JsonSchema2020Test, UnevaluatedPropertiesAcrossApplicators) {
    std::string error;
    auto schema = JsonValue::Parse(R"({
        "allOf":[{"properties":{"a":{"type":"string"}}}],
        "anyOf":[{"properties":{"b":{"type":"integer"}}},{"properties":{"c":{"type":"integer"}}}],
        "unevaluatedProperties":false
    })");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"a":"s","b":1})"), schema, error)) << error;
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"a":"s","c":1})"), schema, error)) << error;
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"({"a":"s","b":1,"extra":true})"), schema, error));
}

TEST(JsonSchema2020Test, UnevaluatedItemsWithPrefixItemsAndContains) {
    std::string error;
    auto schema = JsonValue::Parse(R"({
        "prefixItems":[{"type":"string"}],
        "contains":{"type":"integer"},
        "unevaluatedItems":false
    })");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"(["a",1])"), schema, error)) << error;
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"(["a",true])"), schema, error));
}

TEST(JsonSchema2020Test, DependentSchemasContributeAnnotations) {
    std::string error;
    auto schema = JsonValue::Parse(R"({
        "properties":{"a":{}},
        "dependentSchemas":{"a":{"properties":{"b":{"type":"string"}}}},
        "unevaluatedProperties":false
    })");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"a":1,"b":"x"})"), schema, error)) << error;
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"({"a":1,"b":2})"), schema, error));
}

TEST(JsonSchema2020Test, InvalidPatternFailsClosed) {
    std::string error;
    auto schema = JsonValue::Parse(R"({"type":"string","pattern":"([invalid"})");
    EXPECT_FALSE(ModernValid(JsonValue("anything"), schema, error));
    EXPECT_NE(error.find("SL.RegexFailOpen"), std::string::npos) << error;
}

TEST(JsonSchema2020Test, CachedRegexRemainsValidAcrossEvaluations) {
    auto schema = JsonValue::Parse(R"({
        "type":"object",
        "patternProperties":{"^x-":{"type":"string","pattern":"^ok$"}}
    })");
    auto compiled = mcp::detail::SchemaCompiler::Compile(schema);
    std::string error;
    JsonValue instance(JsonValue::object_tag);
    instance["x-name"] = JsonValue("ok");
    EXPECT_TRUE(mcp::detail::EvaluationEngine::Validate(compiled, instance, error)) << error;
    instance["x-name"] = JsonValue("bad");
    EXPECT_FALSE(mcp::detail::EvaluationEngine::Validate(compiled, instance, error));
    EXPECT_NE(error.find("SL.Pattern"), std::string::npos) << error;
    instance["x-name"] = JsonValue("ok");
    EXPECT_TRUE(mcp::detail::EvaluationEngine::Validate(compiled, instance, error)) << error;
}

TEST(JsonSchema2020Test, RemoteRefRejectedByDefault) {
    std::string error;
    auto schema = JsonValue::Parse(R"({"$ref":"https://example.com/s.json"})");
    EXPECT_FALSE(ModernValid(JsonValue(1), schema, error));
    EXPECT_NE(error.find("SL.RemoteRef"), std::string::npos) << error;
}

TEST(JsonSchema2020Test, RegistryRefResolvesOffline) {
    std::string error;
    mcp::detail::SchemaOptions options;
    options.registry["https://example.com/name.json"] = JsonValue::Parse(
        R"({"$id":"https://example.com/name.json","type":"string"})");
    auto schema = JsonValue::Parse(R"({"$ref":"https://example.com/name.json"})");
    EXPECT_TRUE(ModernValid(JsonValue("x"), schema, error, options)) << error;
    EXPECT_FALSE(ModernValid(JsonValue(1), schema, error, options));
}

TEST(JsonSchema2020Test, DynamicRefUsesOutermostDynamicAnchorInScope) {
    std::string error;
    mcp::detail::SchemaOptions options;
    options.registry["https://example.com/tree.json"] = JsonValue::Parse(
        R"({"$id":"https://example.com/tree.json","$dynamicAnchor":"node",)"
        R"("type":"object","properties":{"data":true,"children":{)"
        R"("type":"array","items":{"$dynamicRef":"#node"}}}})");
    auto strict = JsonValue::Parse(
        R"({"$id":"https://example.com/strict.json","$dynamicAnchor":"node",)"
        R"("$ref":"https://example.com/tree.json","unevaluatedProperties":false})");
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"({"data":1,"typo":2})"), strict, error, options));
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"data":1,"children":[{"data":2}]})"),
                            strict, error, options)) << error;
    auto permissive = JsonValue::Parse(
        R"({"$ref":"https://example.com/tree.json"})");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"({"data":1,"typo":2})"),
                            permissive, error, options)) << error;
}

TEST(JsonSchema2020Test, RelativeRefResolvesAgainstNestedId) {
    std::string error;
    mcp::detail::SchemaOptions options;
    options.registry["http://localhost:1234/baseUriChange/folderInteger.json"] =
        JsonValue::Parse(R"({"type":"integer"})");
    auto schema = JsonValue::Parse(
        R"({"$id":"http://localhost:1234/","items":{)"
        R"("$id":"baseUriChange/","items":{"$ref":"folderInteger.json"}}})");
    EXPECT_TRUE(ModernValid(JsonValue::Parse(R"([[1]])"), schema, error, options)) << error;
    EXPECT_FALSE(ModernValid(JsonValue::Parse(R"([["a"]])"), schema, error, options));
}

TEST(JsonSchema2020Test, UnicodePropertyEscapeTranslatedToRe2ShortAlias) {
    std::string error;
    auto schema = JsonValue::Parse(R"({"type":"string","pattern":"^\\p{Letter}+$"})");
    EXPECT_TRUE(ModernValid(JsonValue("Hello"), schema, error)) << error;
    EXPECT_FALSE(ModernValid(JsonValue("123"), schema, error));
}

TEST(JsonSchema2020Test, BudgetsFailClosed) {
    std::string error;
    mcp::detail::SchemaOptions options;
    options.budgets.max_schema_depth = 1;
    auto deep = JsonValue::Parse(R"({"allOf":[{"allOf":[{"type":"string"}]}]})");
    EXPECT_FALSE(ModernValid(JsonValue("x"), deep, error, options));
    EXPECT_NE(error.find("SL.SchemaDepth"), std::string::npos) << error;

    options = {};
    options.budgets.max_pattern_bytes = 3;
    auto pattern = JsonValue::Parse(R"({"pattern":"abcd"})");
    EXPECT_FALSE(ModernValid(JsonValue("abcd"), pattern, error, options));
    EXPECT_NE(error.find("SL.RegexPatternBytes"), std::string::npos) << error;

    options = {};
    options.budgets.max_subject_bytes = 2;
    auto subject = JsonValue::Parse(R"({"pattern":"a"})");
    EXPECT_FALSE(ModernValid(JsonValue("abcd"), subject, error, options));
    EXPECT_NE(error.find("SL.RegexSubjectBytes"), std::string::npos) << error;
}

TEST(JsonSchema2020Test, UnsupportedDialectAndKeywordFailClosed) {
    std::string error;
    auto dialect = JsonValue::Parse(R"({"$schema":"https://json-schema.org/draft/2019-09/schema"})");
    EXPECT_FALSE(ModernValid(JsonValue(1), dialect, error));
    EXPECT_NE(error.find("SL.UnsupportedDialect"), std::string::npos) << error;
    auto keyword = JsonValue::Parse(R"({"unsupportedKeyword":true})");
    EXPECT_FALSE(ModernValid(JsonValue(1), keyword, error));
    EXPECT_NE(error.find("SL.UnsupportedKeyword"), std::string::npos) << error;
}

TEST(JsonSchema2020Test, LegacyModePreservedAndShadowReportsDivergence) {
    std::string error;
    auto legacy_only = JsonValue::Parse(R"({"unevaluatedProperties":false})");
    EXPECT_TRUE(mcp::detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), legacy_only, error,
        mcp::detail::SchemaMode::Legacy));
    EXPECT_FALSE(mcp::detail::ValidateJsonSchema(
        JsonValue::Parse(R"({"a":1})"), legacy_only, error,
        mcp::detail::SchemaMode::Modern2020));
    auto shadow = mcp::detail::CompareJsonSchemaEngines(
        JsonValue::Parse(R"({"a":1})"), legacy_only);
    EXPECT_TRUE(shadow.legacy_valid);
    EXPECT_FALSE(shadow.modern_valid);
    EXPECT_TRUE(shadow.diverged);
}

#ifdef MCP_JSON_SCHEMA_SUITE_DIR
std::vector<std::filesystem::path> JsonFilesIn(const std::filesystem::path& directory,
                                                bool recursive) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(directory)) return files;
    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

TEST(JsonSchema2020TestSuite, OfficialValidation2020_12) {
    if (std::getenv("MCP_RUN_JSON_SCHEMA_SUITE") == nullptr) {
        GTEST_SKIP() << "set MCP_RUN_JSON_SCHEMA_SUITE=1 to run the pinned official suite";
    }
    const std::filesystem::path root(MCP_JSON_SCHEMA_SUITE_DIR);
    size_t groups = 0;
    size_t cases = 0;
    size_t skipped = 0;
    size_t failures = 0;
    for (const auto& path : JsonFilesIn(root / "validation" / "tests", false)) {
        if (path.filename().string().find("-legacy.json") != std::string::npos) {
            ++skipped;
            continue;
        }
        JsonValue file = JsonValue::Parse(ReadFileText(path));
        const JsonValue* group_tests = file.Find("tests");
        if (!group_tests || !group_tests->IsArray()) continue;
        for (const auto& test_case : group_tests->GetArray()) {
            if (!IsCompatible(test_case)) { ++skipped; continue; }
            if (HasTag(test_case, "format-assertion")) { ++skipped; continue; }
            const JsonValue* schema = test_case.Find("schema");
            const JsonValue* tests = test_case.Find("tests");
            if (!schema || !tests || !tests->IsArray()) continue;
            mcp::detail::SchemaOptions options;
            if (const JsonValue* external = test_case.Find("externalSchemas")) {
                if (!external->IsObject()) continue;
                for (const auto& [uri, document] : external->GetObject()) {
                    options.registry[uri] = document;
                }
            }
            const std::string group_name = test_case.Find("description")
                ? test_case.Find("description")->GetString() : "";
            mcp::detail::CompiledSchema compiled;
            std::string compile_error;
            try {
                compiled = mcp::detail::SchemaCompiler::Compile(*schema, options);
            } catch (const std::exception& error) {
                compile_error = error.what();
            }
            ++groups;
            if (!compiled.Valid()) {
                ++failures;
                if (failures <= 40) {
                    ADD_FAILURE() << path.filename().string() << " / " << group_name
                                  << " / compile: " << compile_error;
                }
                continue;
            }
            for (const auto& test : tests->GetArray()) {
                const JsonValue* data = test.Find("data");
                const JsonValue* expected = test.Find("valid");
                if (!data || !expected || !expected->IsBool()) continue;
                ++cases;
                std::string error;
                bool actual = mcp::detail::EvaluationEngine::Validate(compiled, *data, error);
                if (actual != expected->GetBool()) {
                    ++failures;
                    if (failures <= 40) {
                        const JsonValue* description = test.Find("description");
                        ADD_FAILURE() << path.filename().string() << " / " << group_name
                                      << " / " << (description ? description->GetString() : "")
                                      << ": " << error;
                    }
                }
            }
        }
    }
    EXPECT_GT(groups, size_t{0});
    EXPECT_GT(cases, size_t{0});
    EXPECT_EQ(failures, size_t{0}) << "groups=" << groups << " cases=" << cases
                                   << " skipped=" << skipped;
}
#endif

}
