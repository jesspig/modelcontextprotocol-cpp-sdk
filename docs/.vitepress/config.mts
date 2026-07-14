import { defineConfig } from 'vitepress'

const en = {
  nav: [
    { text: 'Guide', link: '/en/guide/quick-start' },
    { text: 'Server', link: '/en/server/overview' },
    { text: 'Client', link: '/en/client/overview' },
    { text: 'GitHub', link: 'https://github.com/modelcontextprotocol/cpp-sdk' },
  ],
  sidebar: [
    { text: 'Getting Started', items: [
      { text: 'What is MCP?', link: '/en/guide/what-is-mcp' },
      { text: 'Installation', link: '/en/guide/installation' },
      { text: 'Quick Start', link: '/en/guide/quick-start' },
      { text: 'Architecture', link: '/en/guide/architecture' },
      { text: 'Transports', link: '/en/guide/transports' },
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
    ]},
    { text: 'Examples', items: [
      { text: 'Echo Server', link: '/en/examples/echo-server' },
      { text: 'Weather Server', link: '/en/examples/weather-server' },
    ]},
  ],
}

const zh = {
  nav: [
    { text: '指南', link: '/zh/guide/quick-start' },
    { text: '服务端', link: '/zh/server/overview' },
    { text: '客户端', link: '/zh/client/overview' },
    { text: 'GitHub', link: 'https://github.com/modelcontextprotocol/cpp-sdk' },
  ],
  sidebar: [
    { text: '快速入门', items: [
      { text: '什么是 MCP？', link: '/zh/guide/what-is-mcp' },
      { text: '安装', link: '/zh/guide/installation' },
      { text: '快速开始', link: '/zh/guide/quick-start' },
      { text: '架构', link: '/zh/guide/architecture' },
      { text: '传输层', link: '/zh/guide/transports' },
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
  base: '/',

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

  socialLinks: [
    { icon: 'github', link: 'https://github.com/modelcontextprotocol/cpp-sdk' },
  ],

  footer: { message: 'MIT License' },
})
