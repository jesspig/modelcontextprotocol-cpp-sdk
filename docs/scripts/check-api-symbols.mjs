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

function findIn(units, ref) {
  const [scope, member] = ref.split('::')
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
  const scopeName = ref.split('::')[0]
  if (snippetLocalTypes.has(scopeName)) {
    return { status: 'LOCAL', detail: `${scopeName} 是文档片段内自定义的演示类型，非 SDK 符号` }
  }
  const publicHit = findIn(corpus, ref)
  if (publicHit.evidence) {
    return { status: 'HIT', evidence: publicHit.evidence }
  }
  const [scope, member] = ref.split('::')
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
const results = [...references.keys()]
  .sort()
  .map((ref) => ({ ref, occurrences: references.get(ref), ...(exemptReasons.has(ref)
    ? { status: 'EXEMPT', detail: exemptReasons.get(ref) }
    : resolve(ref)) }))

const hits = results.filter((item) => item.status === 'HIT')
const misses = results.filter((item) => item.status === 'MISS')
const exempted = results.filter((item) => item.status === 'EXEMPT')
const locals = results.filter((item) => item.status === 'LOCAL')

console.log(
  `check-api-symbols: 扫描 ${docFiles.length} 个文档/片段文件，` +
    `提取 ${results.length} 个唯一 Scope::member 引用，` +
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

console.log(
  `\n汇总：命中 ${hits.length}，未命中 ${misses.length}，豁免 ${exempted.length}，片段自定义类型 ${locals.length}`
)

if (misses.length > 0) {
  process.exit(1)
}
process.exit(0)
