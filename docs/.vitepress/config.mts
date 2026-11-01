import { defineConfig } from 'vitepress'

const githubLink = 'https://github.com/jesspig/modelcontextprotocol-cpp-sdk'

// socialLinks/footer 属于 themeConfig，写在站点根配置不会生效
const sharedTheme = {
  search: { provider: 'local' },
  socialLinks: [{ icon: 'github', link: githubLink }],
  footer: { message: 'MIT License' },
}

const en = {
  ...sharedTheme,
  nav: [
    { text: 'Guide', link: '/en/guide/quick-start' },
    { text: 'Server', link: '/en/server/overview' },
    { text: 'Client', link: '/en/client/overview' },
    { text: 'GitHub', link: githubLink },
  ],
  sidebar: [
    { text: 'Getting Started', items: [
      { text: 'What is MCP?', link: '/en/guide/what-is-mcp' },
      { text: 'Installation', link: '/en/guide/installation' },
      { text: 'Quick Start', link: '/en/guide/quick-start' },
      { text: 'Architecture', link: '/en/guide/architecture' },
      { text: 'Transports', link: '/en/guide/transports' },
    ]},
    { text: 'Start', items: [
      { text: 'Installation', link: '/en/start/installation' },
      { text: 'Quick Start', link: '/en/start/quickstart' },
    ]},
    { text: 'How-to', items: [
      { text: 'Register a tool', link: '/en/howto/register-a-tool' },
      { text: 'Validate tool input', link: '/en/howto/validate-tool-input' },
      { text: 'Serve a resource', link: '/en/howto/serve-a-resource' },
      { text: 'Serve a prompt', link: '/en/howto/serve-a-prompt' },
      { text: 'Collect user input', link: '/en/howto/collect-user-input' },
      { text: 'Request extra input (MRTR)', link: '/en/howto/request-extra-input' },
      { text: 'Run long tasks', link: '/en/howto/run-long-tasks' },
      { text: 'Report progress', link: '/en/howto/report-progress' },
      { text: 'Send log notifications', link: '/en/howto/send-log-notifications' },
      { text: 'Run a Streamable HTTP server', link: '/en/howto/run-a-streamable-http-server' },
      { text: 'Connect to a server', link: '/en/howto/connect-to-a-server' },
      { text: 'Call tools and read resources', link: '/en/howto/call-tools-and-read-resources' },
      { text: 'Cancel a request', link: '/en/howto/cancel-a-request' },
      { text: 'Handle server-initiated requests', link: '/en/howto/handle-server-initiated-requests' },
    ]},
    { text: 'Learn', items: [
      { text: 'What is MCP?', link: '/en/learn/what-is-mcp' },
      { text: 'Architecture', link: '/en/learn/architecture' },
      { text: 'Transports', link: '/en/learn/transports' },
      { text: 'Protocol Versions', link: '/en/learn/protocol-versions' },
      { text: 'Extensions', link: '/en/learn/extensions' },
    ]},
    { text: 'Reference', items: [
      { text: 'Overview', link: '/en/reference/' },
      { text: 'Server', link: '/en/reference/server' },
      { text: 'Client', link: '/en/reference/client' },
      { text: 'Transports', link: '/en/reference/transports' },
      { text: 'Types', link: '/en/reference/types' },
      { text: 'Errors', link: '/en/reference/errors' },
      { text: 'Process & Storage', link: '/en/reference/process-and-storage' },
      { text: 'Auth & Extensions', link: '/en/reference/auth-and-extensions' },
    ]},
    { text: 'Server', items: [
      { text: 'Overview', link: '/en/server/overview' },
      { text: 'Tools', link: '/en/server/tools' },
      { text: 'Resources', link: '/en/server/resources' },
      { text: 'Prompts', link: '/en/server/prompts' },
      { text: 'Elicitation', link: '/en/server/elicitation' },
      { text: 'Extensions', link: '/en/server/extensions' },
    ]},
    { text: 'Client', items: [
      { text: 'Overview', link: '/en/client/overview' },
      { text: 'OAuth', link: '/en/client/oauth' },
    ]},
    { text: 'Advanced', items: [
      { text: 'Protocol Versions', link: '/en/advanced/protocol-versions' },
      { text: 'MRTR', link: '/en/advanced/mrtr' },
      { text: 'Logging', link: '/en/advanced/logging' },
    ]},
    { text: 'Migration', items: [
      { text: '0.3.9 Migration Guide', link: '/en/migration/0.3.9' },
      { text: '0.3.10 Release Notes', link: '/en/migration/0.3.10' },
    ]},
    { text: 'Examples', items: [
      { text: 'Echo Server', link: '/en/examples/echo-server' },
      { text: 'Weather Server', link: '/en/examples/weather-server' },
    ]},
  ],
}

const zh = {
  ...sharedTheme,
  nav: [
    { text: '指南', link: '/zh/guide/quick-start' },
    { text: '服务端', link: '/zh/server/overview' },
    { text: '客户端', link: '/zh/client/overview' },
    { text: 'GitHub', link: githubLink },
  ],
  sidebar: [
    { text: '快速入门', items: [
      { text: '什么是 MCP？', link: '/zh/guide/what-is-mcp' },
      { text: '安装', link: '/zh/guide/installation' },
      { text: '快速开始', link: '/zh/guide/quick-start' },
      { text: '架构', link: '/zh/guide/architecture' },
      { text: '传输层', link: '/zh/guide/transports' },
    ]},
    { text: '开始使用', items: [
      { text: '安装与集成', link: '/zh/start/installation' },
      { text: '快速上手', link: '/zh/start/quickstart' },
    ]},
    { text: '任务手册', items: [
      { text: '注册一个工具', link: '/zh/howto/register-a-tool' },
      { text: '校验工具入参', link: '/zh/howto/validate-tool-input' },
      { text: '暴露资源', link: '/zh/howto/serve-a-resource' },
      { text: '提供提示', link: '/zh/howto/serve-a-prompt' },
      { text: '向用户收集输入', link: '/zh/howto/collect-user-input' },
      { text: '请求补充输入（MRTR）', link: '/zh/howto/request-extra-input' },
      { text: '跑长任务', link: '/zh/howto/run-long-tasks' },
      { text: '上报进度', link: '/zh/howto/report-progress' },
      { text: '发送协议日志', link: '/zh/howto/send-log-notifications' },
      { text: '跑起 Streamable HTTP 服务端', link: '/zh/howto/run-a-streamable-http-server' },
      { text: '连接服务端', link: '/zh/howto/connect-to-a-server' },
      { text: '调用工具与读取资源', link: '/zh/howto/call-tools-and-read-resources' },
      { text: '取消请求与超时', link: '/zh/howto/cancel-a-request' },
      { text: '承接服务端发起的请求', link: '/zh/howto/handle-server-initiated-requests' },
    ]},
    { text: '核心概念', items: [
      { text: '什么是 MCP？', link: '/zh/learn/what-is-mcp' },
      { text: '架构', link: '/zh/learn/architecture' },
      { text: '传输层', link: '/zh/learn/transports' },
      { text: '协议版本', link: '/zh/learn/protocol-versions' },
      { text: '扩展', link: '/zh/learn/extensions' },
    ]},
    { text: 'API 参考', items: [
      { text: '总览', link: '/zh/reference/' },
      { text: '服务端', link: '/zh/reference/server' },
      { text: '客户端', link: '/zh/reference/client' },
      { text: '传输', link: '/zh/reference/transports' },
      { text: '类型', link: '/zh/reference/types' },
      { text: '错误', link: '/zh/reference/errors' },
      { text: '进程与存储', link: '/zh/reference/process-and-storage' },
      { text: '认证与扩展', link: '/zh/reference/auth-and-extensions' },
    ]},
    { text: '服务端', items: [
      { text: '概述', link: '/zh/server/overview' },
      { text: '工具', link: '/zh/server/tools' },
      { text: '资源', link: '/zh/server/resources' },
      { text: '提示', link: '/zh/server/prompts' },
      { text: '启发式收集', link: '/zh/server/elicitation' },
      { text: '扩展', link: '/zh/server/extensions' },
    ]},
    { text: '客户端', items: [
      { text: '概述', link: '/zh/client/overview' },
      { text: 'OAuth', link: '/zh/client/oauth' },
    ]},
    { text: '进阶', items: [
      { text: '协议版本', link: '/zh/advanced/protocol-versions' },
      { text: 'MRTR 多轮往返', link: '/zh/advanced/mrtr' },
      { text: '日志', link: '/zh/advanced/logging' },
    ]},
    { text: '迁移', items: [
      { text: '0.3.9 迁移指南', link: '/zh/migration/0.3.9' },
      { text: '0.3.10 发布说明', link: '/zh/migration/0.3.10' },
    ]},
    { text: '示例', items: [
      { text: 'Echo 服务端', link: '/zh/examples/echo-server' },
      { text: '天气服务端', link: '/zh/examples/weather-server' },
    ]},
  ],
}

export default defineConfig({
  title: 'MCP C++ SDK',
  description: 'C++17 implementation of the Model Context Protocol',
  base: '/modelcontextprotocol-cpp-sdk/',

  locales: {
    en: {
      label: 'English',
      lang: 'en-US',
      link: '/en/',
      themeConfig: en,
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      link: '/zh/',
      themeConfig: zh,
    },
  },
})
