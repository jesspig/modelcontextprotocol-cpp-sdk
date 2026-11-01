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
