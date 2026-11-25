# 协议版本

本页是整套文档里**唯一**维护双时代差异的地方：其余页面一律回链本页，不复制表格。SDK 用同一个门面、两套编解码器同时支持两个协议时代，因此任何「某能力是否可用」的结论都必须带时代。

## 支持的五个版本

| 版本 | 时代 | 备注 |
|------|------|------|
| `2024-11-05` | 旧 | 首次稳定握手形态 |
| `2025-03-26` | 旧 | 声明缺失时的默认协商值 |
| `2025-06-18` | 旧 | 引入根目录变更通知的下限版本 |
| `2025-11-25` | 旧 | 旧时代上限，也是 `initialize` 未知版本时的回退值 |
| `2026-07-28` | 现代 | 唯一属于现代时代的版本 |

支持表默认即这五个，可用服务端配置里的版本白名单窄化（同时影响 `server/discover` 应答、`initialize` 候选集与入站 `_meta` 校验）。

## 时代如何判定

判定是一次版本字符串的字典序比较：不小于 `2026-07-28` 即现代时代，否则旧时代。因为版本号是 ISO 日期，字典序与时间序一致，所以「现代」实际等价于「等于 `2026-07-28`」——目前只有一个版本落在现代侧。这个谓词是唯一的时代开关，SDK 内部所有分支都走它，没有第二套判定。

协商结果决定选用哪套编解码器：现代时代用 2026 编解码器，其余用 2025 编解码器。协商完成后会整体替换编解码器，因此切换版本对运行中的消息循环是安全的。

## 握手方式

| 项目 | 旧时代 | 现代时代 |
|------|--------|---------|
| 入口 | `initialize` | `server/discover` |
| 后续动作 | 客户端必须补发 `notifications/initialized` | 无：每个请求自带上下文 |
| 未握手时 | 其余请求一律被判「服务尚未初始化」 | 不存在该状态 |
| 版本声明位置 | `params.protocolVersion` | 每请求 `_meta` 里的版本字段 |
| 能否协商出现代版本 | 不能 | 能 |

最后一行值得展开：`initialize` **永远不会**协商出现代版本。客户端声明现代版本时，服务端回 `2025-11-25`；声明了支持表里没有的非空版本同样回 `2025-11-25`；完全没声明版本时回默认值 `2025-03-26`。现代时代只能经 `server/discover` 进入——反过来，服务端在 `_meta` 显示旧版本时也会把 `server/discover` 判为「方法不存在」。

## 双时代可用性矩阵：请求

编解码器各自持有一份方法白名单，时代外的请求被判「不在本时代」并以方法不存在拒绝。下表逐条对应白名单成员，最后一列是本 SDK 在白名单之外附加的门。

| 方法 | 旧时代 | 现代时代 | 本 SDK 的额外门 |
|------|--------|---------|----------------|
| `tools/list` | 可用 | 可用 | 渐进发现开关决定是缓存全量还是按查询分页 |
| `tools/call` | 可用 | 可用 | 任务执行模式双时代均可，现代时代需对端声明任务扩展 |
| `resources/list` | 可用 | 可用 | 无 |
| `resources/read` | 可用 | 可用 | `ui://` 前缀受 apps 开关门控 |
| `resources/templates/list` | 可用 | 可用 | 无 |
| `prompts/list` | 可用 | 可用 | 无 |
| `prompts/get` | 可用 | 可用 | 无 |
| `completion/complete` | 可用 | 可用 | 未设补全回调时按方法不存在拒绝 |
| `server/extensions/list` | 可用 | 可用 | 应答内容由已声明扩展决定 |
| `initialize` | 可用 | 拒绝 | 现代时代下仅未协商会话可放行 |
| `ping` | 可用 | 拒绝 | 客户端入口已标废弃 |
| `resources/subscribe` | 可用 | 拒绝 | 现代时代改由订阅流承载 |
| `resources/unsubscribe` | 可用 | 拒绝 | 同上 |
| `logging/setLevel` | 可用 | 拒绝 | 现代时代客户端入口抛协议违例而非发请求 |
| `roots/list` | 可用 | 拒绝 | 客户端入口已标废弃 |
| `sampling/createMessage` | 可用 | 拒绝 | 现代时代仅作为多轮往返的输入请求出现 |
| `elicitation/create` | 可用 | 拒绝 | 现代时代改由多轮往返承载 |
| `tasks/get` | 可用 | 可用 | 需任务开关；现代时代另加任务名头校验与扩展声明检查 |
| `tasks/update` | 可用 | 可用 | 同上 |
| `tasks/cancel` | 可用 | 可用 | 同上 |
| `tasks/result` | 可用 | 拒绝 | 无 |
| `tasks/list` | 可用 | 拒绝 | 无 |
| `tasks/delete` | 拒绝 | 可用 | 按 authenticated principal 校验 owner，越权统一回 `TaskNotFound` |
| `server/discover` | 拒绝 | 可用 | `_meta` 显示旧版本时被拒 |
| `subscriptions/listen` | 拒绝 | 可用 | 需订阅总开关（默认开） |
| `skills/list` | 拒绝 | 可用 | 需技能开关与资源能力 |
| `skills/get` | 拒绝 | 可用 | 同上 |
| `resources/directory/read` | 拒绝 | 可用 | 还需声明目录读取 |

覆盖 28 个方法名。三处易误读：任务三方法在两套白名单里都出现，是**双时代交集**，而非「仅旧时代」；`tasks/result` 与 `tasks/list` 才是仅旧时代，`tasks/delete` 是仅现代；`server/extensions/list` 是双时代公共成员。

## 双时代可用性矩阵：通知

| 归属 | 通知名 |
|------|--------|
| 双时代公共（8） | `notifications/cancelled`、`notifications/progress`、`notifications/message`、`notifications/resources/updated`、`notifications/resources/list_changed`、`notifications/tools/list_changed`、`notifications/prompts/list_changed`、`notifications/triggers/event` |
| 仅旧时代（9） | `notifications/initialized`、`notifications/elicitation/complete`、`notifications/roots/list_changed`、六个 `notifications/tasks/status|working|completed|failed|cancelled|input_required` |
| 仅现代时代（2） | `notifications/subscriptions/acknowledged`、`notifications/tasks`（载荷为完整 `DetailedTask`，需先订阅，可用 `taskIds` 过滤） |

覆盖 19 个通知名。**0.3.13 起现代任务通知只发/收单个 `notifications/tasks`**，六个旧任务通知仅旧时代可用；触发器事件通知仍在双时代公共集合内。现代时代另有形状约束：通知体不得带 `id`、`result`、`error`，否则判为非法。根目录变更通知在客户端还额外要求协商版本不低于 `2025-06-18`。细节见 [0.3.13 发布说明](/zh/migration/0.3.13)。

## `_meta` 的位置与强制性

请求对象内部把元数据当作与 `params` 平级的独立字段，落线时统一回填进 `params._meta`——两个时代在这一点上线路形态一致，差别在强制性：

| 项目 | 旧时代 | 现代时代 |
|------|--------|---------|
| 是否必须携带 | 不要求 | 除 `server/discover` 外一律必须，缺失即判非法请求 |
| 回填标记 | 无此概念 | 视图层记录「本次由顶层回填」，用于区分原生与合成 |
| 出站写入内容 | 不写 | 版本、客户端信息、客户端能力三项 |
| 无状态设计的意义 | 依赖会话记忆 | 请求自带上下文，握手是唯一无前置的调用 |

`server/discover` 是唯一豁免：它正是用来建立上下文的引导调用。

## 响应的 `resultType`

旧时代的结果编解码是恒等的：对象原样上线。现代时代要求结果对象含 `resultType`，缺失即判非法响应；出站编码会自动补一个「已完成」标记，并把缓存提示里的存活时间与作用域两个字段提升到顶层。五个列表类响应（工具、资源、资源模板、提示、技能）的 `resultType` 还必须是「已完成」——列表结果不允许是多轮往返的部分结果。

## 反向请求：`elicitation_id` 与多轮往返

服务端向客户端发起的用户输入收集在两个时代是两条完全不同的路径，`elicitation_id` 只属于旧时代：

- 旧时代：服务端直发 `elicitation/create`。URL 模式必须带 `elicitation_id`，缺失即被判参数非法；客户端应答后还要补发 `notifications/elicitation/complete` 通知。该字段仅在非表单模式下序列化，现代时代根本不写出。
- 现代时代：`elicitation/create` 不在白名单内。服务端的收集入口改为抛出「需要输入」信号，把收集请求放进多轮往返的输入请求集合，由客户端在同一请求-响应链上补齐输入后重试。URL 模式不生成也不要求 `elicitation_id`，不发完成通知。

采样同理：旧时代可由服务端直发，现代时代只能作为多轮往返的输入请求出现，且服务端会先确认对端声明过采样能力，未声明则回「缺少必需的客户端能力」。

## 错误码重映射

三个传输层内部错误码在现代时代出站时被重映射为内部错误（`-32603`）并记一条告警：请求超时（`-32001`）、连接被拒（`-32003`）、TLS 握手失败（`-32004`）。目的是不把内部故障伪装成协议级错误。四个真正的协议错误码原样透传：头不匹配（`-32020`）、缺少必需的客户端能力（`-32021`）、不支持的协议版本（`-32022`）、需要 URL 输入收集（`-32042`）。其中不支持的协议版本会附带 `data.supported` 版本数组，供客户端做 corrective 重试；`-32042` 目前只登记在透传白名单里，SDK 内没有抛出点。旧时代不做任何重映射。

## 客户端三种连接模式

| 模式 | 行为 | 落到的时代 |
|------|------|-----------|
| Auto（默认） | 探测 `server/discover`，按传输类别决定回退或抛错 | 探测成功为现代，回退为旧 |
| Legacy | 强制走 `initialize` 握手 | 旧 |
| Pin | 不发任何握手请求，直接用钉住的版本构造一个空的发现结果 | 现代 |

Pin 模式跳过协商，也就跳过了服务端能力与信息的获取——能力表是空的，只有确定对端行为时才该用。探测失败的回退规则按传输分类，见 [/zh/learn/transports](/zh/learn/transports)。

## 2026-07-28 修订的破坏性变更

按 2026-07-28 规范修订，每条变更一句话概述并附迁移法。0.3.8 → 0.3.9 公共头文件破坏项的完整旧/新写法见 [0.3.9 迁移指南](/zh/migration/0.3.9)；0.3.12 已发布内容见 [0.3.12 发布说明](/zh/migration/0.3.12)；最新版的行为收紧与逐项迁移见 [0.3.13 发布说明](/zh/migration/0.3.13)。

- **URL elicitation 现代语义**——`url` elicitation 仅遵循现代时代语义。迁移：现代时代下 `ElicitUrl` 不再等待 `elicitationId` + `notifications/elicitation/complete`，而转为 MRTR 输入往返（`inputRequests["elicitation"]`，配置 `request_state_key` 时带 `requestState` 签名），经 `tasks/update` `inputResponses` 履行，客户端用 `SetUrlElicitationHandler` 承接（默认拒绝）；遗留 `elicitationId` 等待路径不变。
- **`_meta` 省略**——空的每请求 `_meta` 改为线上省略，不再发送空对象。迁移：对端须把缺失的 `params._meta` 视为“无元数据”，不得假设空对象必到；发送侧无需补空对象；遗留 `SendRequest` 在 `params` 为空/null 时直接丢弃该字段。
- **重定向收紧**——HTTP 重定向处理比遗留行为更严格。迁移：默认策略 `SameOriginOnly`（同 scheme/host/port，允许 `http`→`https` 升级；跨源跳转被拒绝且剥离 `Authorization` 头，`301`/`302` POST 转 GET，`303` 一律转 GET）；经 `SseClientTransportOptions` / `HttpClientTransportOptions` 的 `redirect_policy`（`FollowAll`/`NoFollow`）与 `max_redirects`（默认 5）逐传输覆盖。
- **默认上限**——对端省略对应限制时适用内置默认上限。迁移：HTTP 响应 8 MB（内部 `HttpRequestSpec` 字段 `max_response_bytes`）、SSE 事件 1 MB（SSE/Streamable HTTP 客户端选项的 `max_sse_event_bytes`）、CIMD 文档 1 MB（硬上限，best-effort 回退）、stdio 帧 8 MB（`StdioClientTransportOptions::max_frame_bytes` / `StdioServerTransport` 构造参数）、服务端请求体 4 MB（`max_request_body_bytes`）、工具参数 `ServerOptions::max_tool_input_elements`（超限返回 `is_error` 工具结果而非协议错误）、技能 manifest 512 文件 / 16 MB 总量（客户端 `CheckSkillLimits`）；对端确需超限时调大对应旋钮，或对载荷分片/分页。
- **异步取消**——`notifications/cancelled` 投递异步，与被取消调用无序。迁移：待处理请求回调现跑在分离线程（仅线程创建失败时回退内联），调用方须按最终一致处理取消——handler 保持幂等并轮询 `RequestContext::IsCancellationRequested()` 协作退出，不得假设取消先于响应到达。

## 新增协议方法时的义务

- 两套白名单都要改：只加一边，另一端会被「不在本时代」拒掉，表现为方法不存在。
- 通知白名单同理，且现代时代的通知体不得带响应字段。
- 旧时代能直发的反向请求，在现代时代必须改走多轮往返的输入请求，否则对端无法应答。
- 带任务语义的方法要考虑现代时代的请求头校验与客户端扩展声明检查。
- 响应结构变化要同时满足现代时代的 `resultType` 要求，列表类方法不能返回部分结果。

## 延伸阅读

- [/zh/reference/errors](/zh/reference/errors) — 完整错误码表与数据载荷形态
- [/zh/reference/types](/zh/reference/types) — 请求与结果类型字段表
- [/zh/reference/client](/zh/reference/client) — 连接模式与协商入口签名
- [/zh/learn/transports](/zh/learn/transports) — 探测回退按传输分类
- [/zh/learn/extensions](/zh/learn/extensions) — 扩展开关与方法白名单的叠加效果
- [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) — 两种时代落点不同的取消
- [/zh/howto/collect-user-input](/zh/howto/collect-user-input) — 反问在两个时代的形态差异
