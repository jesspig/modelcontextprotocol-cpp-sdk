# JSON Schema 校验参考

本页说明 SDK 内两套 JSON Schema 校验入口的行为边界。两者都在 `detail/` 内部层：`ValidateJsonSchema` 的三参数旧入口在 `src/detail/JsonSchemaValidator.hpp`（内联），新 2020-12 引擎的声明在 `src/detail/SchemaValidation.hpp`，实现在 `src/detail/SchemaValidation.cpp`。它们不是随 `include/mcp/` 安装的用户 API；本页记录其语义，供阅读源码与排查校验差异使用。错误码全表见 [错误码参考](/zh/reference/errors)，`JsonValue` 的字段表见 [类型总表](/zh/reference/types)。

::: warning
`detail::ValidateJsonSchema` 三参数形式的默认行为**未改变**：它仍是旧校验器，`unevaluatedProperties`/`unevaluatedItems` 被忽略、非法 `pattern` 被跳过、数值比较经 `double`。需要 2020-12 语义必须显式选择新引擎，不存在“自动升级”。
:::

## 两套入口

| 入口 | 位置 | 默认 | 语义 |
|---|---|---|---|
| `ValidateJsonSchema(instance, schema, error_out)` | `src/detail/JsonSchemaValidator.hpp` | 旧入口、legacy 行为 | 保留原 draft 行为；非法 pattern 不使 schema 失败，unevaluated 关键字被忽略，数值比较走 `double` |
| `ValidateJsonSchema(instance, schema, error_out, SchemaMode::Legacy, options?)` | `src/detail/SchemaValidation.hpp` | 显式 legacy | 与三参数入口等价，便于在同一调用点做模式切换 |
| `ValidateJsonSchema(instance, schema, error_out, SchemaMode::Modern2020, options?)` / `ValidateJsonSchema2020(...)` | `src/detail/SchemaValidation.hpp` | 显式新引擎 | 内部 2020-12 编译 + 求值核心；编译期拒绝不支持的方言、非法 pattern 与无法解析的引用 |
| `SchemaCompiler::Compile(schema, options?)` → `CompiledSchema`；`EvaluationEngine::Validate(compiled, instance, error_out)` | `src/detail/SchemaValidation.hpp` | — | 编译一次、多次求值；`CompiledSchema` 自持 schema 副本与 RE2 编译结果，不可拷贝、可移动 |
| `CompareJsonSchemaEngines(instance, schema, options?)` | `src/detail/SchemaValidation.hpp` | — | shadow 对比：同时跑两套引擎并返回 `SchemaShadowResult{legacy_valid, modern_valid, diverged}`，不改变任何一侧的判定 |

`SchemaMode` 取值为 `Legacy` 与 `Modern2020`。新增的默认参数一律不改变旧行为：旧入口继续返回旧结果，新能力必须被显式点名。

## 新引擎的 2020-12 语义

- **引用与作用域**：离线构建资源/anchor/dynamicAnchor 图；支持 `$ref`、`$dynamicRef`、`$recursiveRef` 与动态作用域。`$dynamicRef` 仅在初始目标自带同名 `$dynamicAnchor` 时进入动态解析，并从作用域最外层向外匹配，避免把中途离开的作用域误算进来。
- **注解与未求值关键字**：`unevaluatedProperties` / `unevaluatedItems` 合并相邻 applicator（`allOf`/`anyOf`/`oneOf`/`if`/`then`/`else`/`dependentSchemas`/`contains` 等）的成功注解；`prefixItems`、`dependentRequired`、`dependentSchemas`、`minContains`/`maxContains` 均按 2020-12 处理。
- **精确数值**：用十进制定点/有理表示做比较，`9007199254740993` 与 `9007199254740992` 不合并，`multipleOf: 0.1` 对 `0.3` 成立，负零与整数零相等；大指数受预算约束，超界即失败关闭。
- **format / content**：默认只产生 annotation，不自动断言或解码。只有显式打开 `SchemaOptions::assert_format` 才对少数 format 做断言；`contentEncoding`/`contentMediaType`/`contentSchema` 默认不解码、不校验内容。
- **失败关闭**：编译期校验方言、关键字白名单、引用可解析性与正则可编译性；超限或不受支持一律抛 `McpError(InvalidParams, ...)`，错误串带 `SL.*` 分类器与 schema 位置，不返回“部分通过”。

## RE2 接入与固定版本

| 项 | 冻结值 / 说明 |
|---|---|
| RE2 | `2025-11-05 @ 927f5d53caf8111721e734cf24724686bb745f55`，BSD-3-Clause；`.gitmodules` 记官方 GitHub URL 与该 commit |
| Abseil | `20260817.0 @ 2065f4ded0558c6f89fee67c8e5228feb4eb960e`，Apache-2.0；`ABSL_MSVC_STATIC_RUNTIME=OFF`，CRT 与 SDK 逐档一致 |
| 构建选项 | `RE2_INSTALL=OFF`（否则 `install(EXPORT)` 引用未导出 absl 目标而失败）、`RE2_BUILD_TESTING=OFF`、`RE2_USE_ICU=OFF`、`USEPCRE=OFF`、`BUILD_SHARED_LIBS=OFF` |
| 匹配语义 | `RE2::PartialMatch` 对应 JSON Schema 的 search 语义；UTF-8；`max_mem` 显式设为 8388608 字节，不依赖默认值 |
| 方言限制 | RE2 不是完整 ECMA-262：无 lookaround/backreference；非法 pattern 直接使 schema 编译失败（旧入口的 fail-open 行为只属于 legacy） |
| 兼容翻译 | 适配层把常见 ECMA-262 长属性名映射到 RE2 内置短别名（`\p{Letter}`→`\p{L}`、`\p{digit}`→`\p{Nd}` 等）；未知长名原样保留并由 RE2 拒绝，不为此启用 ICU |

RE2 与 Abseil 类型不进入任何公共头文件；它们只在 `src/detail/SchemaValidation.cpp` 内部使用。

## 离线引用与注册表

- 默认完全离线：`http`/`https`/`file` 等非片段引用一律拒绝，不解析 DNS、不发起网络请求；错误信息带 `SL.RemoteRef` 与原始引用。
- 需要外部文档时，通过 `SchemaOptions::registry`（URI → `JsonValue`）显式注册；注册表文档也计入文档数/字节预算，并在编译期参与引用解析。
- `$id` 支持相对 URI 与 dot-segment 归一化；引用解析按 RFC 3986 规则相对当前资源 base 进行，fragment 先做百分号解码再按 JSON Pointer 或 anchor 查找。

## 资源预算（T-03 B-37..B-51）

预算在 `SchemaBudgets` 中给出整数阈值；超限在分配/求值前终止并释放资源，错误码统一映射 `InvalidParams` (-32602)，分类器为 `SL.*`。

| ID | 项 | 阈值 | ID | 项 | 阈值 |
|---|---|---:|---|---|---:|
| B-37 | schema 单文档字节 | 1048576 B | B-45 | `uniqueItems` 元素数 | 1024 个 |
| B-38 | schema 总字节 | 4194304 B | B-46 | 引用展开/求值节点 | 100000 个 |
| B-39 | 外部文档数（含主文档） | 16 个 | B-47 | RE2 pattern 字节 | 4096 B |
| B-40 | 引用图深度 | 64 层 | B-48 | RE2 subject 字节 | 65536 B |
| B-41 | schema 结构深度 | 32 层 | B-49 | RE2 `max_mem` | 8388608 B |
| B-42 | schema 节点数 | 100000 个 | B-50 | RE2 编译缓存条目 | 1024 条 |
| B-43 | 求值步数 | 1000000 步 | B-51 | 正则编译失败放行数 | 0 条 |
| B-44 | 求值时间 | 2000 ms | | | |

## 不支持的方言与 `$vocabulary`

`$schema` 只接受标准 2020-12 的 http/https URI。自定义 metaschema 与 `$vocabulary`（例如“无 validation vocabulary”的 metaschema）在**编译阶段**被拒绝，错误形如 `SL.UnsupportedDialect: schema dialect '<URI>' is not supported; custom metaschemas and $vocabulary are rejected at compile time at #`。这是有意的 fail-closed：不支持动态启用/停用 vocab 时，静默忽略会导致假通过。

## 官方 Test Suite 状态

固定 suite commit `e2a68a15cd490750fd6b95e604e4054d7d978d1f`。驱动需显式设置 `MCP_JSON_SCHEMA_SUITE_DIR` 编译定义，并以 `MCP_RUN_JSON_SCHEMA_SUITE=1` 启用；默认 ctest 中该用例为 Skipped。最近一次实跑：405 组 / 1300 例 / 142 跳过 / 2 失败。剩余 2 例均为自定义 metaschema 的 `$vocabulary` 设计性 fail-closed，已记录，**不声称官方 suite 完整通过**。

## 本页对应源码

- `src/detail/SchemaValidation.hpp`、`src/detail/SchemaValidation.cpp`
- `src/detail/JsonSchemaValidator.hpp`、`src/detail/schema/ExactNumber.hpp`
- `tests/unit/JsonSchema2020Tests.cpp`（本地正负例与官方 suite 驱动）
- `third_party/re2/`、`third_party/abseil-cpp/`（固定版本源码）
