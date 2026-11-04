import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const docsDir = path.resolve(scriptDir, '..')
const locales = ['zh', 'en']
const skippedDirNames = new Set(['node_modules', 'snippets', 'scripts'])

function listMarkdown(localeDir) {
  const found = new Set()
  if (!fs.existsSync(localeDir)) {
    return found
  }
  const walk = (dir) => {
    const entries = fs
      .readdirSync(dir, { withFileTypes: true })
      .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
    for (const entry of entries) {
      if (entry.name.startsWith('.') || skippedDirNames.has(entry.name)) {
        continue
      }
      const full = path.join(dir, entry.name)
      if (entry.isDirectory()) {
        walk(full)
      } else if (entry.isFile() && entry.name.endsWith('.md')) {
        found.add(path.relative(localeDir, full).split(path.sep).join('/'))
      }
    }
  }
  walk(localeDir)
  return found
}

function readLines(file) {
  return fs.readFileSync(file, 'utf8').split(/\r?\n/)
}

function isFence(line) {
  return /^ {0,3}(`{3,}|~{3,})/.test(line)
}

function isBoundary(line) {
  return /^(---|\.\.\.)\s*$/.test(line)
}

// 只取 ##/###：中英文档不做字面翻译比对，比较标题层级序列足以暴露整章缺失
function outlineOf(file) {
  const lines = readLines(file)
  const outline = []
  let inFrontmatter = false
  let inFence = false
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index]
    if (index === 0 && isBoundary(line)) {
      inFrontmatter = true
      continue
    }
    if (inFrontmatter) {
      if (isBoundary(line)) {
        inFrontmatter = false
      }
      continue
    }
    if (isFence(line)) {
      inFence = !inFence
      continue
    }
    if (inFence) {
      continue
    }
    const heading = /^(#{2,3})[ \t]+\S/.exec(line)
    if (heading) {
      outline.push({
        level: heading[1].length,
        line: index + 1,
        text: line.replace(/^#+[ \t]+/, '').trim(),
      })
    }
  }
  return outline
}

const localeDirs = locales.map((locale) => path.join(docsDir, locale))
const missingLocaleDirs = localeDirs.filter((dir) => !fs.existsSync(dir))
if (missingLocaleDirs.length > 0) {
  console.error(`check-parity: 缺少语言目录 ${missingLocaleDirs.map((d) => rel(d)).join(', ')}`)
  process.exit(1)
}

function rel(absolute) {
  return path.relative(path.resolve(docsDir, '..'), absolute).split(path.sep).join('/')
}

const fileSets = locales.map((locale, i) => listMarkdown(localeDirs[i]))
const allNames = new Set([].concat(...fileSets.map((set) => [...set])))
const onlyByLocale = locales.map((locale, i) =>
  [...fileSets[i]].filter((name) => fileSets.some((set, j) => j !== i && !set.has(name)))
)

const outlineProblems = []
for (const name of [...allNames].sort()) {
  const paths = localeDirs.map((dir) => path.join(dir, name))
  if (!paths.every((p) => fs.existsSync(p))) {
    continue
  }
  const outlines = paths.map((p) => outlineOf(p))
  const shared = Math.min(...outlines.map((o) => o.length))
  for (let position = 0; position < shared; position += 1) {
    const levels = outlines.map((o) => o[position].level)
    if (new Set(levels).size !== 1) {
      outlineProblems.push(
        `${name}: 第 ${position + 1} 个标题层级不一致 ` +
          outlines
            .map((o, i) => `${locales[i]}=${o[position].level}(${locales[i]}:${o[position].line})`)
            .join(' ')
      )
    }
  }
  for (let localeIndex = 0; localeIndex < outlines.length; localeIndex += 1) {
    const outline = outlines[localeIndex]
    const otherLengths = outlines.filter((_, i) => i !== localeIndex).map((o) => o.length)
    const shortestOther = Math.min(...otherLengths)
    for (let position = shortestOther; position < outline.length; position += 1) {
      const otherLocales = locales.filter((_, i) => i !== localeIndex).join('/')
      outlineProblems.push(
        `${rel(path.join(localeDirs[localeIndex], name))}:${outline[position].line} ` +
          `${otherLocales} 侧缺 ${outline[position].level} 级标题：${outline[position].text}`
      )
    }
  }
}

const missingReports = []
onlyByLocale.forEach((names, localeIndex) => {
  for (const name of names.sort()) {
    const otherLocales = locales.filter((_, i) => i !== localeIndex)
    missingReports.push(
      `${rel(path.join(localeDirs[localeIndex], name))} 缺少 ${otherLocales.join('/')} 对应文件`
    )
  }
})

const sharedCount = [...allNames].filter((name) =>
  localeDirs.every((dir) => fs.existsSync(path.join(dir, name)))
).length

console.log(`check-parity: docs/zh 与 docs/en 共 ${allNames.size} 个页面，${sharedCount} 个同名成对`)
console.log(
  `集合对称：${missingReports.length === 0 ? '是（每个 zh 页面都有 en 页面，反之亦然）' : '否'}`
)
console.log(`标题结构差异：${outlineProblems.length} 处`)

for (const report of missingReports) {
  console.log(`  - ${report}`)
}
for (const report of outlineProblems) {
  console.log(`  - ${report}`)
}

if (missingReports.length > 0 || outlineProblems.length > 0) {
  process.exit(1)
}
process.exit(0)
