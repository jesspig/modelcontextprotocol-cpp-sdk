import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const docsDir = path.resolve(scriptDir, '..')
const repoRoot = path.resolve(docsDir, '..')
const includeDir = path.join(repoRoot, 'include')

const headerExtensions = new Set(['.hpp', '.h', '.hh'])
// 非本 SDK 符号：标准库/第三方命名空间、CMake 目标名
const externalScopes = new Set(['std', 'boost', 'chrono', 'OpenSSL', 'fmt', 'ranges', 'nlohmann'])
// 需要放行时在此登记并写明理由；理由会打印在 CI 输出里
const exemptRefs = [
  {
    ref: 'ClientOptions::enable_apps_extension',
    reason: '0.3.9 已删除字段，迁移页与发布说明必须点名其不存在',
  },
  {
    ref: 'InputRequiredConfig::legacy_shim',
    reason: '0.3.9 已删除字段，迁移页与发布说明必须点名其不存在',
  },
  {
    ref: 'ServerOptions::enable_dpop',
    reason: '0.3.9 已删除字段，迁移页与发布说明必须点名其不存在',
  },
  {
    ref: 'ServerOptions::enable_interceptor_alias',
    reason: '0.3.9 已删除字段，迁移页与发布说明必须点名其不存在',
  },
  {
    ref: 'ProcessHandle::Terminate',
    reason: '内部平台抽象，用于说明 shutdown_timeout 的去处，未列入接口表',
  },
  {
    ref: 'Rev2026Codec::EncodeErrorCode',
    reason: 'src/protocol/WireCodec.cpp 内的私有编解码器，仅用于解释时代重映射',
  },
  {
    ref: 'Rev2026Codec::ValidateNotification',
    reason: '私有编解码器，仅用于解释入站校验位置',
  },
  {
    ref: 'Rev2026Codec::ValidateRequest',
    reason: '私有编解码器，仅用于解释入站校验位置',
  },
  {
    ref: 'Rev2026Codec::ValidateResponse',
    reason: '私有编解码器，仅用于解释响应校验与 resultType 要求',
  },
  {
    ref: 'detail::JoinThreadSafely',
    reason: '内部线程工具，用于说明 IO 回调内 Close 的 self-join 规避',
  },
  {
    ref: 'detail::WriteAtomicSecure',
    reason: '内部安全写辅助，迁移页记录其引入',
  },
  {
    ref: 'detail::WriteWholeMessage',
    reason: '内部整帧写辅助，仅用于解释 stdio 写入行为',
  },
]

function walk(dir, predicate) {
  const collected = []
  if (!fs.existsSync(dir)) {
    return collected
  }
  const entries = fs
    .readdirSync(dir, { withFileTypes: true })
    .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
  for (const entry of entries) {
    if (entry.name.startsWith('.') || entry.name === 'node_modules') {
      continue
    }
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      collected.push(...walk(full, predicate))
    } else if (entry.isFile() && predicate(full)) {
      collected.push(full)
    }
  }
  return collected
}

function relative(absolute) {
  return path.relative(repoRoot, absolute).split(path.sep).join('/')
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function tokenPattern(token) {
  return new RegExp(`\\b${escapeRegExp(token)}\\b`)
}

function loadUnit(file) {
  const text = fs.readFileSync(file, 'utf8')
  return { file: relative(file), text, lines: text.split(/\r?\n/) }
}

// include/**/detail/ 是未纳入公共 API 契约的内部头，仅在未命中时作为附加证据报告
const headerUnits = walk(includeDir, (file) => headerExtensions.has(path.extname(file))).map(loadUnit)
const corpus = headerUnits.filter((unit) => !unit.file.includes('/detail/'))
const internalCorpus = headerUnits.filter((unit) => unit.file.includes('/detail/'))

if (corpus.length === 0) {
  console.error(`check-api-symbols: ${relative(includeDir)} 下未找到任何公共头文件`)
  process.exit(1)
}

const docFiles = [
  ...walk(path.join(docsDir, 'zh'), (file) => path.extname(file) === '.md'),
  ...walk(path.join(docsDir, 'en'), (file) => path.extname(file) === '.md'),
  ...walk(path.join(docsDir, 'snippets'), (file) => path.extname(file) === '.cpp'),
]

const declaredNamespaces = new Set(
  headerUnits.flatMap((unit) => [...unit.text.matchAll(/\bnamespace\s+([A-Za-z_][A-Za-z0-9_]*)/g)].map((m) => m[1]))
)

// 文档片段里自造的演示类型不是 SDK 符号，单独归类而不是算未命中
const snippetUnits = walk(path.join(docsDir, 'snippets'), (file) => path.extname(file) === '.cpp')
  .map(loadUnit)
const snippetLocalTypes = new Set(
  snippetUnits.flatMap((unit) => [
    ...unit.text.matchAll(/\b(?:enum\s+class|enum|struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)/g),
  ].map((m) => m[1]))
)

const isTypeName = (value) => value[0] >= 'A' && value[0] <= 'Z'

// (?!\*) 跳过 methods::k* 之类通配写法；非大写字母开头的限定名只有确实是 namespace 时才检查，
// 以免把 json::object() 这种"本仓库不存在的名字"反例说明当成引用
const referencePattern = /\b([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)(?!\*)/g
const references = new Map()

for (const file of docFiles) {
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    for (const match of line.matchAll(referencePattern)) {
      const [scope, member] = [match[1], match[2]]
      if (externalScopes.has(scope)) {
        continue
      }
      if (!isTypeName(scope) && !declaredNamespaces.has(scope)) {
        continue
      }
      const ref = `${scope}::${member}`
      if (!references.has(ref)) {
        references.set(ref, [])
      }
      references.get(ref).push({ file: relative(file), line: index + 1 })
    }
  })
}

// 点号形式（Options.field）也计入：左侧必须是公共头里真的声明过的类型名，
// 并排除路径与文件名（`McpClient.cpp` 这类）造成的假阳性
const typeNames = new Set(
  headerUnits.flatMap((unit) => [
    ...unit.text.matchAll(/\b(?:struct|class|enum class)\s+(?:MCP_API\s+)?([A-Z][A-Za-z0-9_]*)/g),
  ].map((m) => m[1]))
)
const fileExtensions = new Set([
  'cpp', 'cxx', 'cc', 'h', 'hpp', 'hh', 'md', 'txt', 'rst', 'yaml', 'yml', 'json',
  'js', 'mjs', 'ts', 'tsx', 'mts', 'sh', 'bash', 'ps1', 'cmake', 'in', 'txt', 'log',
  'png', 'svg', 'jpg', 'html', 'css', 'lock', 'gitignore', 'editorconfig',
])
const dottedPattern = /(^|[^/\w.])([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)(?![\w./])/g

for (const file of docFiles) {
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    for (const match of line.matchAll(dottedPattern)) {
      const [, , scope, member] = match
      if (!typeNames.has(scope) || fileExtensions.has(member.toLowerCase())) {
        continue
      }
      const ref = `${scope}.${member}`
      if (references.has(`${scope}::${member}`)) {
        continue
      }
      if (!references.has(ref)) {
        references.set(ref, [])
      }
      references.get(ref).push({ file: relative(file), line: index + 1 })
    }
  })
}

function splitRef(ref) {
  const separator = ref.indexOf('::') >= 0 ? '::' : '.'
  const index = ref.indexOf(separator)
  return [ref.slice(0, index), ref.slice(index + separator.length)]
}

function canonicalRef(ref) {
  return ref.replace(/\./, '::')
}

function findIn(units, ref) {
  const [scope, member] = splitRef(ref)
  const scopePattern = tokenPattern(scope)
  const memberPattern = tokenPattern(member)
  const scopeFiles = units.filter((unit) => scopePattern.test(unit.text))
  for (const unit of scopeFiles) {
    const lineIndex = unit.lines.findIndex((line) => memberPattern.test(line))
    if (lineIndex >= 0) {
      return { evidence: `${unit.file}:${lineIndex + 1}` }
    }
  }
  return { scopeFiles }
}

function resolve(ref) {
  const scopeName = splitRef(ref)[0]
  if (snippetLocalTypes.has(scopeName)) {
    return { status: 'LOCAL', detail: `${scopeName} 是文档片段内自定义的演示类型，非 SDK 符号` }
  }
  const publicHit = findIn(corpus, ref)
  if (publicHit.evidence) {
    return { status: 'HIT', evidence: publicHit.evidence }
  }
  const [scope, member] = splitRef(ref)
  const internalHit = findIn(internalCorpus, ref)
  if (internalHit.evidence) {
    return {
      status: 'MISS',
      detail: `仅命中内部头 ${internalHit.evidence}，不属于公共 API`,
    }
  }
  if (publicHit.scopeFiles.length === 0) {
    return {
      status: 'MISS',
      detail: internalHit.scopeFiles.length > 0
        ? `${scope} 只出现在内部头 ${internalHit.scopeFiles.map((unit) => unit.file).slice(0, 3).join(', ')}，公共头文件无此声明`
        : `${scope} 未出现在 include/ 下任何头文件（私有实现符号或已删除）`,
    }
  }
  return {
    status: 'MISS',
    detail: `${scope} 声明于 ${publicHit.scopeFiles.map((unit) => unit.file).slice(0, 3).join(', ')}，其中未见成员 ${member}`,
  }
}

const exemptReasons = new Map(
  exemptRefs.map((entry) => [entry.ref, entry.reason])
)
const isExempt = (ref) => exemptReasons.has(ref) || exemptReasons.has(canonicalRef(ref))
const results = [...references.keys()]
  .sort()
  .map((ref) => ({ ref, occurrences: references.get(ref), ...(isExempt(ref)
    ? { status: 'EXEMPT', detail: exemptReasons.get(ref) ?? exemptReasons.get(canonicalRef(ref)) }
    : resolve(ref)) }))

const hits = results.filter((item) => item.status === 'HIT')
const misses = results.filter((item) => item.status === 'MISS')
const exempted = results.filter((item) => item.status === 'EXEMPT')
const locals = results.filter((item) => item.status === 'LOCAL')

console.log(
  `check-api-symbols: 扫描 ${docFiles.length} 个文档/片段文件，` +
    `提取 ${results.length} 个唯一 Scope::member / Options.field 引用，` +
    `公共头文件 ${corpus.length} 个（内部头 ${internalCorpus.length} 个不计入命中）`
)

console.log('\n未命中：')
if (misses.length === 0) {
  console.log('  （无）')
}
for (const miss of misses) {
  console.log(`  MISS ${miss.ref} — ${miss.detail}`)
  for (const occurrence of miss.occurrences) {
    console.log(`       引用处 ${occurrence.file}:${occurrence.line}`)
  }
}

if (exempted.length > 0) {
  console.log('\n显式豁免：')
  for (const item of exempted) {
    console.log(`  EXEMPT ${item.ref} — ${item.detail}`)
  }
}

console.log('\n命中：')
for (const hit of hits) {
  console.log(`  HIT ${hit.ref} -> ${hit.evidence}`)
}

// 片段 region 双向完整性：页面引用的 region 必须存在，存在的 region 必须被引用，
// 且标记必须顶格——VitePress 只识别行首的 `// region`，缩进会让标记本身被渲染进代码块
const snippetDir = path.join(docsDir, 'snippets')
const usedRegions = new Map()
for (const file of docFiles) {
  if (!file.endsWith('.md')) {
    continue
  }
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    const match = /<<< +@\/snippets\/([A-Za-z0-9._-]+\.cpp)#([A-Za-z0-9_-]+)/.exec(line)
    if (!match) {
      return
    }
    const key = `${match[1]}#${match[2]}`
    if (!usedRegions.has(key)) {
      usedRegions.set(key, [])
    }
    usedRegions.get(key).push({ file: relative(file), line: index + 1 })
  })
}

const declaredRegions = new Set()
const markerProblems = []
for (const file of walk(snippetDir, (candidate) => candidate.endsWith('.cpp'))) {
  const base = path.basename(file)
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    const marker = /^(\s*)\/\/\s+(region|endregion)(\s+\S+)?\s*$/.exec(line)
    if (!marker) {
      return
    }
    if (marker[1].length > 0) {
      markerProblems.push(`${relative(file)}:${index + 1} region 标记必须顶格书写：${line.trim()}`)
    }
    if (marker[2] === 'region' && marker[3]) {
      declaredRegions.add(`${base}#${marker[3].trim()}`)
    }
  })
}

const regionProblems = []
const howtoFenceProblems = []
for (const file of docFiles) {
  if (!file.endsWith('.md') || !/[\\/]howto[\\/]/.test(file)) {
    continue
  }
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    if (/^ {0,3}```(cpp|c\+\+)\b/.test(line)) {
      howtoFenceProblems.push(
        `${relative(file)}:${index + 1} 任务手册正文禁止手写 C++ 围栏，代码必须来自 docs/snippets 的 region`
      )
    }
  })
}
howtoFenceProblems.sort()

// 围栏必须带语言标注：裸 ``` 在 VitePress 里渲染成无标签代码块，输出块应写 ```text
const bareFenceProblems = []
for (const file of docFiles) {
  if (!file.endsWith('.md')) {
    continue
  }
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  let inFence = false
  lines.forEach((line, index) => {
    const marker = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line)
    if (!marker) {
      return
    }
    if (!inFence) {
      inFence = true
      if (marker[2].trim() === '') {
        bareFenceProblems.push(
          `${relative(file)}:${index + 1} 开围栏缺少语言标注，输出块请写 \`\`\`text`
        )
      }
    } else {
      inFence = false
    }
  })
}
bareFenceProblems.sort()

// 源码引用门禁：行号只允许出现在 examples/ 走读页（那里是「跟我读这段代码」，发版时可与 tag 对齐）；
// 其余页面写行号会随源码改动静默失真。引用到的文件名还必须真实存在，防止改名后继续留在文档里
const citedExtensions = 'cpp|hpp|hh|cc|cmake|sh|json|yaml|mts|txt'
const citationPattern = new RegExp(
  `([A-Za-z0-9_./-]+\\.(?:${citedExtensions})(?:\\.in)?)(:(\\d+)(?:-\\d+)?)?`,
  'g'
)
const strayRefPattern = /`:\d+(?:-\d+)?`|(?:[、，,]\s*)`(?<![\d-])\d{1,5}-\d{1,5}(?![-\d])`/g
// 安装期才生成的导出物，仓库里没有同名文件但文档必须提到
const generatedCitations = new Set(['mcpTargets.cmake', 'mcpConfig.cmake', 'mcp.pc'])
const indexableRoots = ['src', 'include', 'tests', 'examples', 'scripts', 'cmake', 'docs', '.github']
const knownFileNames = new Set(
  fs
    .readdirSync(repoRoot, { withFileTypes: true })
    .filter((entry) => entry.isFile())
    .map((entry) => entry.name)
)
for (const root of indexableRoots) {
  for (const candidate of walk(path.join(repoRoot, root), () => true)) {
    knownFileNames.add(path.basename(candidate))
  }
}
const lineRefProblems = []
const citationProblems = []
for (const file of docFiles) {
  if (!file.endsWith('.md')) {
    continue
  }
  const isWalkthrough = /(^|[\\/])(?:zh|en)[\\/]examples[\\/]/.test(file)
  fs
    .readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .forEach((rawLine, index) => {
      const line = rawLine.replace(/https?:\/\/\S+/g, '')
      if (!isWalkthrough) {
        for (const match of line.matchAll(strayRefPattern)) {
          lineRefProblems.push(
            `${relative(file)}:${index + 1} 非示例页不得引用源码行号（源码一改即失真）：${match[0].trim()}`
          )
        }
      }
      for (const match of line.matchAll(citationPattern)) {
        const target = match[1]
        if (match[2] && !isWalkthrough) {
          lineRefProblems.push(
            `${relative(file)}:${index + 1} 非示例页不得引用源码行号（源码一改即失真）：${target}${match[2]}`
          )
        }
        if (target.startsWith('.') || generatedCitations.has(target)) {
          continue
        }
        if (target.startsWith('@/')) {
          if (!fs.existsSync(path.join(docsDir, target.slice(2)))) {
            citationProblems.push(
              `${relative(file)}:${index + 1} 引用的片段不存在：${target}`
            )
          }
          continue
        }
        if (target.startsWith('/')) {
          // `<<< @/snippets/x.cpp` 捕获时丢了前导 @，剩下的斜杠按 docs 根解析
          if (!fs.existsSync(path.join(docsDir, target.slice(1)))) {
            citationProblems.push(
              `${relative(file)}:${index + 1} 引用的片段不存在：${target}`
            )
          }
          continue
        }
        if (target.startsWith('mcp/')) {
          if (!fs.existsSync(path.join(includeDir, target))) {
            citationProblems.push(
              `${relative(file)}:${index + 1} 引用的公共头不存在：${target}`
            )
          }
          continue
        }
        if (target.includes('/')) {
          if (!fs.existsSync(path.join(repoRoot, target))) {
            citationProblems.push(
              `${relative(file)}:${index + 1} 引用的路径不存在：${target}`
            )
          }
          continue
        }
        if (!knownFileNames.has(target)) {
          citationProblems.push(
            `${relative(file)}:${index + 1} 引用的文件名在仓库中找不到：${target}`
          )
        }
      }
    })
}
lineRefProblems.sort()
citationProblems.sort()
for (const [key, occurrences] of usedRegions) {
  if (!declaredRegions.has(key)) {
    const where = occurrences.map((item) => `${item.file}:${item.line}`).join(', ')
    regionProblems.push(`${key} 被页面引用，但片段里没有该 region（引用处 ${where}）`)
  }
}
for (const key of declaredRegions) {
  if (!usedRegions.has(key)) {
    regionProblems.push(`${key} 已定义但没有任何页面引用（死代码）`)
  }
}
regionProblems.sort()
markerProblems.sort()

console.log(
  `\n片段 region：定义 ${declaredRegions.size} 个，被引用 ${usedRegions.size} 处，` +
    `完整性问题 ${regionProblems.length} 处，标记书写问题 ${markerProblems.length} 处，` +
    `任务手册手写代码块 ${howtoFenceProblems.length} 处`
)
console.log(
  `围栏标注：缺语言标注的开围栏 ${bareFenceProblems.length} 处`
)
console.log(
  `源码引用：非示例页行号引用 ${lineRefProblems.length} 处，无法定位的文件引用 ${citationProblems.length} 处`
)
for (const problem of regionProblems) {
  console.log(`  REGION ${problem}`)
}
for (const problem of markerProblems) {
  console.log(`  MARKER ${problem}`)
}
for (const problem of howtoFenceProblems) {
  console.log(`  FENCE ${problem}`)
}
for (const problem of bareFenceProblems) {
  console.log(`  LANG ${problem}`)
}
for (const problem of lineRefProblems.slice(0, 40)) {
  console.log(`  LINEREF ${problem}`)
}
if (lineRefProblems.length > 40) {
  console.log(`  LINEREF …另有 ${lineRefProblems.length - 40} 处同类`)
}
for (const problem of citationProblems.slice(0, 40)) {
  console.log(`  CITE ${problem}`)
}
if (citationProblems.length > 40) {
  console.log(`  CITE …另有 ${citationProblems.length - 40} 处同类`)
}

console.log(
  `\n汇总：命中 ${hits.length}，未命中 ${misses.length}，豁免 ${exempted.length}，片段自定义类型 ${locals.length}`
)

if (
  misses.length > 0 ||
  regionProblems.length > 0 ||
  markerProblems.length > 0 ||
  howtoFenceProblems.length > 0 ||
  bareFenceProblems.length > 0 ||
  lineRefProblems.length > 0 ||
  citationProblems.length > 0
) {
  process.exit(1)
}
process.exit(0)
