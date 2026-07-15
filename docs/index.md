---
lang: en-US
title: MCP C++ SDK
---

<script setup>
import { useRouter } from 'vitepress'
import { onMounted } from 'vue'

const router = useRouter()
onMounted(() => {
  const lang = navigator.language || navigator.userLanguage
  if (lang.startsWith('zh')) {
    router.go('/zh/')
  } else {
    router.go('/en/')
  }
})
</script>

<div style="text-align:center;padding:4rem 0">
  <p>Redirecting based on your browser language...</p>
  <p><a href="/en/">English</a> | <a href="/zh/">简体中文</a></p>
</div>
