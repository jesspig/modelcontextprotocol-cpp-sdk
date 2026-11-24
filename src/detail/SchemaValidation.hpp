#pragma once
#include <mcp/JsonValue.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace mcp { namespace detail {

namespace schema {
struct EvalContext;
class SchemaRepository;
}

enum class SchemaMode {
    Legacy,
    Modern2020
};

enum class SchemaDialect {
    Draft2020_12,
    Legacy
};

struct SchemaBudgets {
    size_t max_document_bytes = 1048576;
    size_t max_total_bytes = 4194304;
    size_t max_documents = 16;
    size_t max_reference_depth = 64;
    size_t max_schema_depth = 32;
    size_t max_schema_nodes = 100000;
    size_t max_evaluation_steps = 1000000;
    size_t max_unique_items = 1024;
    size_t max_expand_nodes = 100000;
    size_t max_pattern_bytes = 4096;
    size_t max_subject_bytes = 65536;
    size_t max_regex_memory = 8388608;
    size_t max_regex_cache_entries = 1024;
    std::chrono::milliseconds max_evaluation_time{2000};
};

struct SchemaOptions {
    SchemaDialect dialect = SchemaDialect::Draft2020_12;
    bool remote_refs_enabled = false;
    bool assert_format = false;
    bool decode_content = false;
    std::map<std::string, JsonValue> registry;
    SchemaBudgets budgets{};
};

class CompiledSchema {
public:
    CompiledSchema();
    ~CompiledSchema();
    CompiledSchema(CompiledSchema&&) noexcept;
    CompiledSchema& operator=(CompiledSchema&&) noexcept;
    CompiledSchema(const CompiledSchema&) = delete;
    CompiledSchema& operator=(const CompiledSchema&) = delete;

    [[nodiscard]] bool Valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class SchemaCompiler;
    friend class EvaluationEngine;
    friend struct schema::EvalContext;
    friend class schema::SchemaRepository;
};

class SchemaCompiler {
public:
    static CompiledSchema Compile(const JsonValue& schema, const SchemaOptions& options = {});
};

class EvaluationEngine {
public:
    static bool Validate(const CompiledSchema& compiled, const JsonValue& instance,
                         std::string& error_out);
};

bool ValidateJsonSchema2020(const JsonValue& instance, const JsonValue& schema,
                            std::string& error_out, const SchemaOptions& options = {});

bool ValidateJsonSchema(const JsonValue& instance, const JsonValue& schema,
                        std::string& error_out, SchemaMode mode,
                        const SchemaOptions& options = {});

struct SchemaShadowResult {
    bool legacy_valid = false;
    bool modern_valid = false;
    bool diverged = false;
};

SchemaShadowResult CompareJsonSchemaEngines(const JsonValue& instance,
                                            const JsonValue& schema,
                                            const SchemaOptions& options = {});

}}
