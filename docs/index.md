---
lang: en-US
title: MCP C++ SDK
---

<script setup>
import { useRouter, withBase } from 'vitepress'
import { onMounted } from 'vue'

const base = withBase('/')
const router = useRouter()
onMounted(() => {
  const lang = navigator.language || navigator.userLanguage
  if (lang.startsWith('zh')) {
    router.go(withBase('/zh/'))
  } else {
    router.go(withBase('/en/'))
  }
})
</script>

<div style="text-align:center;padding:4rem 0">
  <p>Redirecting based on your browser language...</p>
  <p><a :href="withBase('/en/')">English</a> | <a :href="withBase('/zh/')">简体中文</a></p>
</div>
