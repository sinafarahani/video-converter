// One log line, with the file paths in it kept readable.
//
// Log lines are right-to-left Persian paragraphs, and most of them carry a
// path. Left to the bidi algorithm, a path's neutral edge characters move to
// the wrong end of it ("/tmp/a.pdf" shows as "tmp/a.pdf/", "D:\" as "\:D"),
// and Persian folder names inside a path swap places. So each path is drawn in
// its own left-to-right <bdi>, with every name between separators in a nested
// <bdi> that takes its own direction. Only elements are added -- no control
// characters -- so selecting and copying a line gives back exactly its text.

import { h, type FunctionalComponent, type VNode } from 'vue'

export interface LogEntry {
  id: number
  text: string
  /** The exact paths inside `text`, when whoever wrote the line knows them. */
  paths?: string[]
}

interface Segment {
  text: string
  path: boolean
}

// The engine's messages that carry paths (engine/src/messages.cpp); keep the
// two in step. Matching the fixed text around a path picks out exactly the
// path, spaces and Persian names included, which no guess at what a path looks
// like can do. Groups alternate: text, path, text[, path, text].
const TEMPLATES: RegExp[] = [
  /^(عملیات برای فایل)(.+)( شروع شد\.)$/su,
  /^(عملیات برای فایل)(.+)( ناموفق بود\.(?:کد: -?\d+)?)$/su,
  /^(فایل: )(.+)(با موفقیت ذخیره شد\.)$/su,
  /^(خطا در حذف فایل موقت: )(.+)()$/su,
  /^(خطا در هنگام جایگزین کردن فایل: )(.+)()$/su,
  /^(خطا در حذف فایل اصلی: )(.+)()$/su,
  /^(خطا در تغییر نام فایل موقت به فایل اصلی: )(.+)()$/su,
  /^(فایل )(.+)( از قبل کوچکتر از اندازه هدف است \(.*\)، بدون فشرده\u200cسازی منتقل شد\.)$/su,
  /^(فایل یا پوشه ورودی پیدا نشد: )(.+)()$/su,
  /^(فایل )(.+)( ویدیو یا صدا نیست و بدون تغییر رها شد\.)$/su,
  /^(فایلی با همین نام وجود داشت؛ )(.+)( با نام )(.+)( ذخیره می\u200cشود\.)$/su,
  /^(فایل )(.+)( رد شد، چون خروجی )(.+)( از قبل وجود دارد؛ این نام خروجی به چند ورودی تعلق دارد و ممکن است آن فایل از ورودی دیگری ساخته شده باشد\.)$/su,
]

// Anything else -- an error text, say -- gets a best guess: a path starts at
// "/", "\\" or a drive letter that does not continue a word or another path,
// and runs to the end of the line. Running long costs little; cutting a path
// short is what garbles it.
const PATH_START = /(?<![\p{L}\p{N}_.:\\/])(?:\/|\\\\|[A-Za-z]:[\\/])/u

// Hebrew and Arabic script, Persian included.
const RTL = /[\u0590-\u08FF\uFB1D-\uFDFF\uFE70-\uFEFF]/

function splitKnown(text: string, paths: string[]): Segment[] {
  const segs: Segment[] = []
  let at = 0
  for (;;) {
    let start = -1
    let len = 0
    for (const p of paths) {
      if (!p) continue
      const i = text.indexOf(p, at)
      if (i >= 0 && (start < 0 || i < start || (i === start && p.length > len))) {
        start = i
        len = p.length
      }
    }
    if (start < 0) break
    if (start > at) segs.push({ text: text.slice(at, start), path: false })
    segs.push({ text: text.slice(start, start + len), path: true })
    at = start + len
  }
  if (at < text.length) segs.push({ text: text.slice(at), path: false })
  return segs
}

function splitTemplate(text: string): Segment[] | null {
  for (const re of TEMPLATES) {
    const m = re.exec(text)
    if (!m) continue
    const segs: Segment[] = []
    for (let g = 1; g < m.length; g++) {
      if (m[g]) segs.push({ text: m[g], path: g % 2 === 0 })
    }
    return segs
  }
  return null
}

function splitGuess(text: string): Segment[] {
  const m = PATH_START.exec(text)
  if (!m) return [{ text, path: false }]
  const tail = text.slice(m.index)
  const path = tail.trimEnd()
  const segs: Segment[] = []
  if (m.index > 0) segs.push({ text: text.slice(0, m.index), path: false })
  segs.push({ text: path, path: true })
  if (path.length < tail.length) segs.push({ text: tail.slice(path.length), path: false })
  return segs
}

function segments(entry: LogEntry): Segment[] {
  if (entry.paths?.length) return splitKnown(entry.text, entry.paths)
  return splitTemplate(entry.text) ?? splitGuess(entry.text)
}

function renderPath(path: string): VNode {
  // A path with no right-to-left letters is already in order inside the
  // left-to-right isolate; only mixed ones need a node per name.
  if (!RTL.test(path)) return h('bdi', { dir: 'ltr' }, path)
  const parts = path.split(/([\\/]+)/).filter(Boolean)
  return h(
    'bdi',
    { dir: 'ltr' },
    parts.map((p) => (/^[\\/]+$/.test(p) ? p : h('bdi', p))),
  )
}

export const LogText: FunctionalComponent<{ entry: LogEntry }> = ({ entry }) =>
  segments(entry).map((s) => (s.path ? renderPath(s.text) : s.text))
LogText.props = ['entry']
