# 提供资源

资源是客户端按 URI 读取的只读数据，形态接近 REST 里的 GET 端点。本篇覆盖静态资源与 URI 模板资源的注册、`resources/read` 的匹配顺序，以及内容变更后的通知投递。

## 前置条件

- 已能让一个服务端实例跑起来，见 [服务端总览](/zh/server/overview)。
- 手上有一个 `std::shared_ptr<ITransport>`；想做进程内自测，`InMemoryTransport::CreatePair()` 会给出成对的 `.client` 与 `.server`。
- 签名与字段表只在参考层维护，本篇不重复：注册入口与注册期校验见 [服务端门面参考](/zh/reference/server)，`ResourceOptions`、`ReadResourceResult`、`ResourceLink` 见 [类型层参考](/zh/reference/types)。

## 第一步：选择静态资源还是模板资源

| 数据形态 | 注册入口 | 出现在哪张列表 | handler 额外收到 |
|---|---|---|---|
| 数量有限、URI 是完整字面量 | `McpServer::RegisterResource` | `resources/list` | 无，只给请求 URI |
| 同构、按 id 或路径分片 | `McpServer::RegisterResourceTemplate` | `resources/templates/list` | 解码后的变量表 |

两类条目进同一张注册表，冲突判定也共用：名字相同，或 URI／URI 模式字符串相同，后一次注册抛 `McpError`（`InvalidParams`，`src/server/McpServer.cpp:805-814`、`:854-863`）。

::: info
任一注册成功即自动声明 `resources` 能力，并把 `subscribe` 与 `list_changed` 一并置真（`src/server/McpServer.cpp:1950-1954`），没有单独关闭的入口。
:::

## 第二步：注册静态资源

```cpp
server->RegisterResource("release-notes", "docs://release-notes",
    ResourceOptions{}
        .Title("Release Notes")
        .Description("Notes for the newest published release")
        .MimeType("text/plain"),
    [](const std::string& uri) -> ReadResourceResult {
        TextResourceContents text;
        text.uri = uri;
        text.mime_type = "text/plain";
        text.text = "1.0.0 - first published release.";

        ReadResourceResult result;
        result.contents.push_back(ResourceContents{std::move(text)});
        return result;
    });
```

- `contents` 的每个变体都要自己填 `uri`，SDK 不回填；一次响应可以给多条。
- 文本走 `TextResourceContents::text`，二进制走 `BlobResourceContents::blob`，按协议放 base64 文本——SDK 既不解码也不校验（`src/core/Content.cpp:92-104`），`mime_type` 必须写对。
- `ResourceOptions` 只覆盖 `title`／`description`／`mime_type`／`icons`；协议字段 `size` 与 `annotations` 没有注册入口，静态列表条目也就带不出它们。

## 第三步：写 URI 模板并读懂变量

```cpp
server->RegisterResourceTemplate("user-profile", "docs://users/{userId}/profile",
    ResourceOptions{}
        .Title("User Profile")
        .Description("One profile document per user id")
        .MimeType("application/json"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        TextResourceContents text;
        text.uri = uri;
        text.mime_type = "application/json";
        text.text = "{\"id\":\"" + vars.at("userId") + "\"}";

        ReadResourceResult result;
        result.contents.push_back(ResourceContents{std::move(text)});
        return result;
    });
```

模板语法是 RFC 6570 的一个子集，全部判定发生在注册期：

| 项目 | 本 SDK 的行为 |
|---|---|
| 操作符 | 支持 `{var}` `{+var}` `{#var}` `{.var}` `{/var}` `{;var}` `{?var}` `{&var}`；尾部 `*` 被接受后忽略 |
| 变量名 | 只有 `A-Za-z0-9_`，允许 `.` 分组与 `%XX` 转义 |
| 前缀修饰 | 不支持 `{var:3}` |
| 规模上限 | 模板 2048 字符、变量 32 个 |
| 结构拒绝 | 花括号不配对、两个以上多段变量、相邻且无字面量分隔的两个变量 |

不合法即抛 `McpError`，消息形如 `invalid resource template '<uri>': <原因>`（`src/server/McpServer.cpp:836-841`）。

命中后交给 handler 的变量值是**百分号解码**之后的结果：请求 `docs://users/a%20b/profile`，`vars.at("userId")` 得到 `a b`。简单变量只接受 unreserved 字符与 `%XX`，出现裸的 `/`、`?`、`#` 就判为整条不匹配；要在一个段里容纳保留字符，用 `{+var}`。

## 第四步：确认读取匹配顺序与未命中形态

`resources/read` 是两段匹配（`src/server/McpServer.cpp:2789-2809`）：先按注册顺序把所有**非模板**条目与请求 URI 做精确等值比较，一条都没中才按注册顺序逐个做模板匹配。

- 静态优先与注册先后无关：模板先注册、静态后注册，请求 `test://x` 仍然走静态 handler（`McpServerTest.ReadResourcePrefersStaticOverTemplate`）。
- 全未命中：`InvalidParams`，消息 `resource not found: <uri>`，`data` 里回显 `{"uri":"<uri>"}`（`src/server/McpServer.cpp:2812-2819`）。
- `McpErrorCode::ResourceNotFound`（`-32002`）只出现在已知码表里，服务端未命中路径没有任何抛出点——对端拿到的永远是 `-32602`。

::: warning
handler 里抛 `McpError` 会原样编码成 JSON-RPC 错误响应；抛其它异常收敛为 `InternalError` + 固定文本 `internal error`，原始 `what()` 只进日志。资源与提示路径的这套规则不同于工具路径，见 [错误码与错误模型](/zh/reference/errors)。
:::

## 第五步：在内容变化时通知订阅者

```cpp
server->SendResourceListChanged();
server->SendResourceUpdated("docs://release-notes");
```

- `SendResourceUpdated(uri)` 只走订阅路由，仅投递给过滤器里列了该 URI 的订阅者；订阅表为空时直接返回，什么都不发（`src/protocol/McpSessionHandler.cpp:979-988`）。
- 实例已 `Run()` 时，注册动作会自己补发一次 `list_changed`（`src/server/McpServer.cpp:819-821`），手写调用只在你主动增删条目之后才需要。
- 旧时代的 `resources/subscribe` 与现代 `subscriptions/listen` 的 `resourceSubscriptions` 写进同一张订阅表，所以 `SendResourceUpdated` 在两个时代都能按 URI 命中；`list_changed` 的投递范围按时代分流（`src/server/McpServer.cpp:953-959`）。

## 第六步：按链接与片段引用资源

把资源以链接形式回给客户端时用 `ResourceLink`：`name` 与 `uri` 缺失、非字符串或空串都会让解析侧抛 `InvalidParams`，`size` 为负同样拒收（`src/core/Content.cpp:237-264`）——注册资源时用的名字与 URI 必须非空，同一套约束会在对端解析你的链接时生效。

客户端只想读一个文件的某一段时用 `FileRef`：`enable_file_input` 打开后，工具参数里的 `{"uri","offset","length"}` 会先按同一条两段匹配规则读整份资源再切片。`offset`／`length` 缺省为 0，一旦给了就必须是整数或整数值浮点且落在 int64 范围内，否则 `DeserializeFailed`（`include/mcp/McpTypes.hpp:162-189`）。

## 完整代码

<<< @/snippets/serve-a-resource.cpp#full

## 验证

```bash
ctest --preset debug -R 'McpServerTest\.(RegisterResource|ReadResource|SendResourceUpdated)|UriTemplateTest\.'
```

31 个用例应全部 `Passed`，末行 `100% tests passed, 0 tests failed out of 31`。三处关键断言可用来确认你读到的是同一份实现：

| 用例 | 断言值 |
|---|---|
| `McpServerTest.ReadResourceTemplateValueIsPctDecoded` | `contents[0].text` 为 `id=a b` |
| `McpServerTest.ReadResourcePrefersStaticOverTemplate` | `contents[0].text` 为 `static` |
| `McpServerTest.ReadResourceMissCarriesUri` | 错误码 `-32602`，`data.uri` 为 `test://missing-resource` |

## 时代差异

`resources/list`、`resources/templates/list`、`resources/read` 与 `resources/updated`／`resources/list_changed` 通知双时代都可用；差别只在订阅的建立方式（`resources/subscribe` 仅旧时代）和 `list_changed` 的投递范围（旧时代广播、现代只给订阅者）。详见 [协议版本](/zh/learn/protocol-versions)。

## 常见错误

| 症状 | 成因 | 处置 |
|---|---|---|
| 注册时抛 `resource already registered` 或 `resource uri already registered` | 静态与模板共用一张冲突表 | 换名字或换 URI 前缀，别指望分表隔离 |
| 空闲期改了内容，客户端毫无反应 | 没有订阅者时 `SendResourceUpdated` 静默丢弃 | 变更后补一次 `SendResourceListChanged()`，或让客户端重读 |
| 模板一注册就抛 `invalid resource template` | 用了 `{var:3}`、相邻变量或多个多段变量 | 补字面量分隔符，整条模板只留一个 `{+var}` |
| 变量值含 `/` 时读不到内容 | 简单变量拒绝保留字符 | 改用 `{+var}`，或在客户端侧做百分号编码 |
| 对端拿到 `-32602` 而不是 `-32002` | 未命中路径就是用 `InvalidParams` | 按消息前缀 `resource not found:` 判读，`data.uri` 已回显 |
| 列表里看不到刚注册的资源 | 分页截断，每页 100 条 | 带 `cursor` 翻页，或把长尾条目改成模板 |

## 下一步

- 参数化模板的取参体验：[提供提示](/zh/howto/serve-a-prompt)
- 让工具参数接收文件片段：[注册一个工具](/zh/howto/register-a-tool)
- 把资源当用户输入的载体：[收集用户输入](/zh/howto/collect-user-input)

## 本页对应源码

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/McpTypes.hpp`（`ResourceOptions`、`ReadResourceResult`、`FileRef`、`SubscriptionFilter`）
- `include/mcp/Content.hpp`（`TextResourceContents`、`BlobResourceContents`、`ResourceLink`）
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp`（注册、`resources/read` 两段匹配、订阅与通知）
- `src/protocol/McpSessionHandler.cpp`（`AddSubscription`、`NotifySubscribers`）
- `src/detail/UriTemplate.hpp`（RFC 6570 子集解析与匹配）
- `src/core/Content.cpp`（`ResourceContents` 与 `ResourceLink` 的序列化校验）
