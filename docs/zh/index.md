---
layout: home

hero:
  name: MCP C++ SDK
  text: C++17 实现的模型上下文协议
  tagline: 从构建到排障的完整手册：工具、资源、提示词、用户输入收集、任务、订阅，双协议时代全覆盖。
  actions:
    - theme: brand
      text: 五分钟跑通
      link: /zh/start/quickstart
    - theme: alt
      text: 安装与集成
      link: /zh/start/installation
    - theme: alt
      text: 在 GitHub 查看
      link: https://github.com/jesspig/modelcontextprotocol-cpp-sdk

features:
  - title: 我要暴露一个工具
    details: 注册 API 的全形态：描述、标题、输入 schema、行为注解、图标，以及结构化输出的兑现义务。
    link: /zh/howto/register-a-tool
  - title: 我要拦住非法入参
    details: 声明 inputSchema 后 SDK 会替你校验，失败回 is_error 与 invalid arguments；也讲手写类型防御的写法。
    link: /zh/howto/validate-tool-input
  - title: 我要对外提供资源与提示
    details: 静态资源、URI 模板、订阅与 list_changed，以及带参数规格的提示词与补全回调。
    link: /zh/howto/serve-a-resource
  - title: 我要向用户追问
    details: 服务端发起 elicitation 的 form 与 url 两种模式，会话定位与三态结果。
    link: /zh/howto/collect-user-input
  - title: 我有长任务要跑
    details: 任务化执行、进度上报与协议日志：task_store、双时代路由、可轮询的结算。
    link: /zh/howto/run-long-tasks
  - title: 我要跑 HTTP 服务端
    details: Streamable HTTP 装配、会话与事件存储、无状态模式与断线恢复。
    link: /zh/howto/run-a-streamable-http-server
  - title: 我要把它放到公网
    details: Bearer 校验、挑战帧与元数据端点、DPoP、Host/Origin 白名单与体积上限。
    link: /zh/howto/protect-an-http-server
  - title: 我要连一个服务端
    details: 五种传输的客户端装配、连接模式与协商、关闭时机与在途请求结算。
    link: /zh/howto/connect-to-a-server
  - title: 我的客户端要带鉴权
    details: OAuth 授权码与 PKCE、客户端凭据、令牌缓存与自动续期、DPoP 证明装配。
    link: /zh/howto/add-oauth-to-a-client
  - title: 请求卡住了
    details: 取消与超时的真实语义、错误码分型与重映射，以及无响应排障顺序。
    link: /zh/howto/cancel-a-request
  - title: 我要搞清两个协议时代
    details: 方法与通知的双时代矩阵、错误码重映射、协商回退规则——唯一权威页。
    link: /zh/learn/protocol-versions
  - title: 我要查签名和字段
    details: 参考层只放表格：方法、Options 全字段、默认值、抛出与对应源文件。
    link: /zh/reference/
---
