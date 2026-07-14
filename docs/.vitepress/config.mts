import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'en-US',
  title: 'MCP C++ SDK',
  description: 'C++17 implementation of the Model Context Protocol',
  base: '/',

  themeConfig: {
    nav: [
      { text: 'Home', link: '/' },
      { text: 'Guide', link: '/getting-started' },
      { text: 'Server', link: '/server/overview' },
      { text: 'Client', link: '/client/overview' },
      { text: 'GitHub', link: 'https://github.com/modelcontextprotocol/cpp-sdk' },
    ],

    sidebar: [
      {
        text: 'Getting Started',
        items: [
          { text: 'What is MCP?', link: '/guide/what-is-mcp' },
          { text: 'Installation', link: '/guide/installation' },
          { text: 'Quick Start', link: '/guide/quick-start' },
          { text: 'Architecture', link: '/guide/architecture' },
          { text: 'Transports', link: '/guide/transports' },
        ],
      },
      {
        text: 'Server',
        items: [
          { text: 'Overview', link: '/server/overview' },
          { text: 'Tools', link: '/server/tools' },
          { text: 'Resources', link: '/server/resources' },
          { text: 'Prompts', link: '/server/prompts' },
          { text: 'Elicitation', link: '/server/elicitation' },
          { text: 'Extensions', link: '/server/extensions' },
        ],
      },
      {
        text: 'Client',
        items: [
          { text: 'Overview', link: '/client/overview' },
          { text: 'OAuth', link: '/client/oauth' },
        ],
      },
      {
        text: 'Advanced',
        items: [
          { text: 'Protocol Versions', link: '/advanced/protocol-versions' },
          { text: 'MRTR', link: '/advanced/mrtr' },
        ],
      },
      {
        text: 'Examples',
        items: [
          { text: 'Echo Server', link: '/examples/echo-server' },
          { text: 'Weather Server', link: '/examples/weather-server' },
        ],
      },
    ],

    socialLinks: [
      { icon: 'github', link: 'https://github.com/modelcontextprotocol/cpp-sdk' },
    ],

    footer: {
      message: 'MIT License',
    },
  },
})
