<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { invoke, subscribe, type EngineEvent, type InputsEvent } from './bridge'
import { LogText, type LogEntry } from './logText'

type OutputMode = 'replace' | 'copy'
type Existing = 'overwrite' | 'skip'
type Speed = 'optimal' | 'fast'
type SizeMode = 'manual' | 'auto'
type Level = 'low' | 'medium' | 'high' | 'extreme'

// Folders and/or audio/video files, in the order given. The engine walks the
// folders and decides what is media; the UI only holds the list.
const inputs = ref<string[]>([])
const outputDir = ref('')
const maxSize = ref('')
const outputMode = ref<OutputMode>('replace')
const existing = ref<Existing>('overwrite')
const speed = ref<Speed>('optimal')
const sizeMode = ref<SizeMode>('manual')
const level = ref<Level>('medium')

const running = ref(false)
const progress = ref(0)
const currentFile = ref('')
const fileIndex = ref(0)
const fileCount = ref(0)
const ffmpegFound = ref(true)

// Per-file detail. The first version showed only the overall percentage,
// which for a folder of two hundred files rounds to "0%" for a full day and
// is indistinguishable from a hang. This is what tells a user the encode is
// alive, how fast it is going, and roughly when the current file will finish.
const fileFraction = ref(0)
const phase = ref('')
const pass = ref(0)
const passCount = ref(0)
const encFps = ref(0)
const encSpeed = ref(0)
const eta = ref(-1)
const elapsed = ref(0)

const logLines = ref<LogEntry[]>([])
const logEl = ref<HTMLElement | null>(null)
// A stable key per line, so dropping the oldest ones below does not make Vue
// redraw every line that is left.
let logId = 0

// The Java version let its log pane grow without limit for the whole session.
// A large batch produces thousands of lines, and every one of them stays in the
// DOM; capping keeps scrolling smooth without losing anything that matters.
const MAX_LOG_LINES = 2000

// `paths` are the exact paths inside `text`, when known, so they can be shown
// left to right inside the Persian line (see logText.ts).
function log(text: string, paths?: string[]) {
  logLines.value.push({ id: logId++, text, paths })
  if (logLines.value.length > MAX_LOG_LINES) {
    logLines.value.splice(0, logLines.value.length - MAX_LOG_LINES)
  }
  nextTick(() => {
    if (logEl.value) logEl.value.scrollTop = logEl.value.scrollHeight
  })
}

// -- derived state ---------------------------------------------------------

const copyMode = computed(() => outputMode.value === 'copy')
const manualMode = computed(() => sizeMode.value === 'manual')

const sizeValid = ref(true)
const sizeHuman = ref('')

watch([maxSize, sizeMode], async () => {
  if (!manualMode.value || maxSize.value.trim() === '') {
    sizeValid.value = true
    sizeHuman.value = ''
    return
  }
  try {
    const r = await invoke<{ valid: boolean; human: string }>('parseSize', { text: maxSize.value })
    sizeValid.value = r.valid
    sizeHuman.value = r.human
  } catch {
    sizeValid.value = false
    sizeHuman.value = ''
  }
})

const multiInput = computed(() => inputs.value.length > 1)

function persianDigits(n: number): string {
  return String(n).replace(/\d/g, (d) => '۰۱۲۳۴۵۶۷۸۹'[Number(d)])
}

// One path stays an ordinary editable field, so typing or pasting a folder
// works exactly as before. A list collapses to a read-only count with every
// path in the tooltip; telling files from folders would need a disk round
// trip, so the summary only counts.
const inputText = computed({
  get: () =>
    multiInput.value
      ? `${persianDigits(inputs.value.length)} مورد انتخاب شده`
      : (inputs.value[0] ?? ''),
  set: (text: string) => {
    if (multiInput.value) return
    // Not trimmed: that would eat the space while typing "My Clip".
    inputs.value = text.trim() ? [text] : []
  },
})
const inputTooltip = computed(() => inputs.value.join('\n'))

// Explorer's "Copy as path" puts quotes around the path, and a multi-selection
// pastes into one line as "a""b" or "a" "b"; surrounding spaces come along
// with a sloppy copy. Applied when the user finishes editing a field (change
// fires on blur and Enter, not per keystroke), and only to what was typed: a
// path from a picker, a drop or the OS is used exactly as given.
function cleanTypedPaths(text: string): string[] {
  const t = text.trim()
  if (/^("[^"]*"\s*)+$/.test(t)) {
    const quoted = [...t.matchAll(/"([^"]*)"/g)].map((m) => m[1]).filter((p) => p.trim())
    return [...new Set(quoted)]
  }
  return t ? [t] : []
}

function commitTypedInput() {
  if (multiInput.value) return
  inputs.value = cleanTypedPaths(inputs.value[0] ?? '')
}

function commitTypedOutput() {
  outputDir.value = cleanTypedPaths(outputDir.value)[0] ?? ''
}

const canStart = computed(() => {
  if (inputs.value.length === 0) return false
  if (copyMode.value && !outputDir.value) return false
  if (manualMode.value && (!maxSize.value || !sizeValid.value)) return false
  return ffmpegFound.value
})

// One decimal below 10% so a long batch visibly moves instead of sitting on 0.
const percent = computed(() => {
  const p = progress.value * 100
  return p < 10 ? p.toFixed(1) : Math.round(p).toString()
})
const filePercent = computed(() => (fileFraction.value * 100).toFixed(1))

const PHASE_LABELS: Record<string, string> = {
  probe: 'بررسی فایل',
  analyze: 'تحلیل صدا',
  pass1: 'مرحله ۱ از ۲',
  pass2: 'مرحله ۲ از ۲',
  encode: 'رمزگذاری',
  remux: 'انتقال بدون فشرده‌سازی',
  audio: 'تبدیل صدا',
}
const phaseLabel = computed(() => PHASE_LABELS[phase.value] ?? phase.value)

function humanDuration(seconds: number): string {
  if (seconds < 0 || !isFinite(seconds)) return '—'
  const s = Math.round(seconds)
  if (s < 60) return `${s} ثانیه`
  if (s < 3600) return `${Math.floor(s / 60)} دقیقه`
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  return m ? `${h} ساعت و ${m} دقیقه` : `${h} ساعت`
}

// "مرحله ۱ از ۲ · ۱۷.۳٪ · ۱.۴ فریم/ثانیه · باقی‌مانده ۲۳ ساعت"
const detailLine = computed(() => {
  if (!running.value || !phase.value) return ''
  const parts: string[] = [phaseLabel.value]
  if (phase.value !== 'probe' && phase.value !== 'analyze') {
    parts.push(`${filePercent.value}٪`)
    if (encFps.value > 0) parts.push(`${encFps.value.toFixed(1)} فریم/ثانیه`)
    if (encSpeed.value > 0) parts.push(`سرعت ${encSpeed.value.toFixed(encSpeed.value < 1 ? 3 : 2)}x`)
    if (eta.value >= 0) parts.push(`باقی‌مانده ${humanDuration(eta.value)}`)
  }
  if (elapsed.value > 0) parts.push(`گذشته ${humanDuration(elapsed.value)}`)
  return parts.join(' · ')
})

// Latin tokens inside an RTL string get reordered by the bidi algorithm --
// "نمونه ورودی: 1GB یا 500MB" renders as "GB 500 یاMBنمونه ورودی: 1" without
// help. U+2066 (LEFT-TO-RIGHT ISOLATE) and U+2069 (POP DIRECTIONAL ISOLATE)
// fence each token off so it keeps its own direction.
const sizePlaceholder = 'نمونه ورودی: ⁦1GB⁩ یا ⁦500MB⁩'

// Selecting "copy to" turns on the destination controls, and defaults the
// collision policy to overwrite -- same as the original.
watch(outputMode, (mode) => {
  if (mode === 'copy') existing.value = 'overwrite'
})

// -- actions ---------------------------------------------------------------

function logRejected(paths: string[]) {
  for (const p of paths) log(`نادیده گرفته شد (ویدیو یا صدا نیست یا پیدا نشد): ${p}`, [p])
}

// Shared by the file picker, drops and paths handed over by the OS. A batch
// in which nothing survived leaves the current list alone rather than wiping
// a selection the user may still want; the rejected lines explain why.
function applyInputs(paths: string[], append: boolean) {
  if (paths.length === 0) return
  inputs.value = [...new Set(append ? [...inputs.value, ...paths] : paths)]
}

async function browse(which: 'input' | 'output') {
  try {
    const r = await invoke<{ cancelled: boolean; path: string }>('pickFolder', {
      title: which === 'input' ? 'پوشه ورودی را انتخاب کنید' : 'پوشه خروجی را انتخاب کنید',
      initial: which === 'input' ? (inputs.value[0] ?? '') : outputDir.value,
    })
    if (r.cancelled || !r.path) return
    if (which === 'input') inputs.value = [r.path]
    else outputDir.value = r.path
  } catch (e) {
    log(String(e))
  }
}

async function pickFiles() {
  try {
    const r = await invoke<{ cancelled: boolean; paths?: string[]; rejected?: string[] }>(
      'pickFiles',
      { title: 'فایل‌های ویدیو یا صدا را انتخاب کنید', initial: inputs.value[0] ?? '' },
    )
    if (r.cancelled) return
    logRejected(r.rejected ?? [])
    applyInputs(r.paths ?? [], false)
  } catch (e) {
    log(String(e))
  }
}

async function toggleStart() {
  if (running.value) {
    await invoke('cancel')
    return
  }
  if (!canStart.value) {
    if (inputs.value.length === 0) log('هیچ فایل یا پوشه‌ای برای ورودی انتخاب نشده است.')
    else if (copyMode.value && !outputDir.value) log('فیلد پوشه خروجی نمیتواند خالی باشد')
    else if (manualMode.value) log('حداکثر اندازه خروجی نامعتبر است.')
    return
  }

  progress.value = 0
  running.value = true

  try {
    const r = await invoke<{ started: boolean }>('start', {
      inputs: [...inputs.value],
      // Still read by the bridge when `inputs` is absent; kept so the field
      // means the same thing to an older native side.
      inputDir: inputs.value[0] ?? '',
      outputDir: outputDir.value,
      outputMode: outputMode.value,
      existing: existing.value,
      speed: speed.value,
      sizeMode: sizeMode.value,
      maxSize: maxSize.value,
      level: level.value,
    })
    if (!r.started) running.value = false
  } catch (e) {
    log(String(e))
    running.value = false
  }
}

// -- event channel ---------------------------------------------------------

let unsubscribe: (() => void) | null = null

// Paths handed over while a batch runs are held back: the input controls are
// locked, and swapping the list under a running job would make the screen
// disagree with what is being converted. They take effect once it ends.
let pendingInputs: { paths: string[]; append: boolean } | null = null

// Explorer starts one process per selected file, so a multi-selection arrives
// as one replacing batch followed by appending ones. When nothing in the
// replacing batch survived (the file vanished, say), the replace is still
// owed: the next batch of the same burst takes its place instead of being
// added to the old, unrelated list. Only an appending batch reads this, and
// one of those comes only right after another batch, so it cannot go stale.
let replaceOwed = false

function onInputs(ev: InputsEvent) {
  logRejected(ev.rejected ?? [])
  const paths = ev.paths ?? []
  if (!ev.append) replaceOwed = paths.length === 0
  if (paths.length === 0) return
  const append = ev.append && !replaceOwed
  replaceOwed = false
  if (!running.value) {
    applyInputs(paths, append)
    return
  }
  if (!pendingInputs) log('ورودی جدید دریافت شد؛ پس از پایان عملیات فعلی جایگزین می‌شود.')
  pendingInputs =
    pendingInputs && append
      ? { paths: [...new Set([...pendingInputs.paths, ...paths])], append: pendingInputs.append }
      : { paths, append }
}

// Watching `running` rather than only the finished event also covers a start
// that the native side refused.
watch(running, (isRunning) => {
  if (isRunning || !pendingInputs) return
  const p = pendingInputs
  pendingInputs = null
  applyInputs(p.paths, p.append)
})

function onEvent(ev: EngineEvent) {
  switch (ev.type) {
    case 'log':
      log(ev.text, ev.paths)
      break
    case 'inputs':
      onInputs(ev)
      break
    case 'progress':
      progress.value = ev.fraction
      fileFraction.value = ev.fileFraction
      currentFile.value = ev.file
      fileIndex.value = ev.index
      fileCount.value = ev.count
      phase.value = ev.phase
      pass.value = ev.pass
      passCount.value = ev.passCount
      encFps.value = ev.fps
      encSpeed.value = ev.speed
      eta.value = ev.eta
      elapsed.value = ev.elapsed
      break
    case 'finished':
      running.value = false
      progress.value = 0
      fileFraction.value = 0
      currentFile.value = ''
      phase.value = ''
      break
  }
}

// -- drag and drop ---------------------------------------------------------
//
// DOM File objects carry only names, so the absolute paths come from C++,
// which records them when the drag enters the window (CefDragHandler) and
// hands them over through takeDroppedPaths. CEF calls OnDragEnter only for an
// Alloy-style browser, which is why app.cc creates the window and browser in
// Alloy style; under Chrome style takeDroppedPaths would always be empty.

const dragging = ref(false)
const dropActive = computed(() => dragging.value && !running.value)

function isFileDrag(e: DragEvent): boolean {
  return e.dataTransfer?.types.includes('Files') ?? false
}

function onDragEnter(e: DragEvent) {
  e.preventDefault()
  if (isFileDrag(e)) dragging.value = true
}

// Cancelling dragover makes the page the drop target and cancelling drop
// stops Chromium's default of opening the dropped file as a page; both are
// needed on every event, including the ones we then ignore.
function onDragOver(e: DragEvent) {
  e.preventDefault()
  const files = isFileDrag(e)
  // Also brings the overlay back if something below cleared it too early.
  if (files) dragging.value = true
  if (e.dataTransfer) {
    e.dataTransfer.dropEffect = files && !running.value ? 'copy' : 'none'
  }
}

// dragleave fires for every element crossed; moving to another element names
// it in relatedTarget, and only leaving the page leaves it empty. (Counting
// enters against leaves goes wrong for good when the element under the
// pointer is removed mid-drag: its dragleave no longer reaches the window.)
function onDragLeave(e: DragEvent) {
  if (e.relatedTarget === null) dragging.value = false
}

// A job ending removes the current-file line; if the pointer leaves the
// window straight from it, that last dragleave is lost as well.
watch(running, () => {
  dragging.value = false
})

async function onDrop(e: DragEvent) {
  e.preventDefault()
  dragging.value = false
  // Only a file drag refreshes the native side's list; asking after any other
  // drop could return the paths of an earlier drag that left the window.
  if (!isFileDrag(e) || running.value) return
  try {
    const r = await invoke<{ paths?: string[]; rejected?: string[] }>('takeDroppedPaths')
    const paths = r.paths ?? []
    const rejected = r.rejected ?? []
    // The paths come from the window's native drag handler. If it saw none,
    // say so rather than let the drop silently do nothing.
    if (paths.length === 0 && rejected.length === 0) {
      log('مسیر فایل‌های رهاشده در دسترس نبود؛ از دکمه «انتخاب فایل» یا «انتخاب پوشه» استفاده کنید.')
      return
    }
    logRejected(rejected)
    applyInputs(paths, false)
  } catch (err) {
    log(String(err))
  }
}

onMounted(async () => {
  window.addEventListener('dragenter', onDragEnter)
  window.addEventListener('dragover', onDragOver)
  window.addEventListener('dragleave', onDragLeave)
  window.addEventListener('drop', onDrop)
  unsubscribe = subscribe(onEvent)
  try {
    const s = await invoke<{ ffmpegFound: boolean; running: boolean }>('getState')
    ffmpegFound.value = s.ffmpegFound
    running.value = s.running
    if (!s.ffmpegFound) log('ffmpeg پیدا نشد. لطفا برنامه را دوباره نصب کنید.')
  } catch (e) {
    log(String(e))
  }
})

onUnmounted(() => {
  window.removeEventListener('dragenter', onDragEnter)
  window.removeEventListener('dragover', onDragOver)
  window.removeEventListener('dragleave', onDragLeave)
  window.removeEventListener('drop', onDrop)
  unsubscribe?.()
})
</script>

<template>
  <div class="shell">
    <main class="card" :class="{ 'drop-target': dropActive }">
      <!-- input: a folder, audio/video files, or a mix -->
      <div class="row">
        <label class="label" for="in">پوشه یا فایل ورودی:</label>
        <input
          id="in"
          v-model="inputText"
          class="field"
          :class="{ multi: multiInput }"
          type="text"
          spellcheck="false"
          autocomplete="off"
          placeholder="یک پوشه یا چند فایل انتخاب کنید، یا آن‌ها را اینجا رها کنید"
          :title="inputTooltip"
          :readonly="multiInput"
          :disabled="running"
          @change="commitTypedInput"
        />
        <button v-if="multiInput" class="btn small" :disabled="running" @click="inputs = []">
          پاک کردن
        </button>
        <button class="btn" :disabled="running" @click="browse('input')">انتخاب پوشه</button>
        <button class="btn" :disabled="running" @click="pickFiles">انتخاب فایل</button>
      </div>

      <!-- size mode -->
      <div class="row">
        <span class="label">حالت اندازه:</span>
        <div class="choices">
          <label class="radio">
            <input v-model="sizeMode" type="radio" value="manual" :disabled="running" />
            <span>دستی</span>
          </label>
          <label class="radio">
            <input v-model="sizeMode" type="radio" value="auto" :disabled="running" />
            <span>خودکار</span>
          </label>
        </div>
        <span class="hint">{{ manualMode ? 'اندازه دقیق خروجی' : 'کیفیت ثابت' }}</span>
      </div>

      <!-- max size (manual only) -->
      <div class="row" :class="{ dimmed: !manualMode }">
        <label class="label" for="size">حداکثر اندازه فایل خروجی:</label>
        <input
          id="size"
          v-model="maxSize"
          class="field short"
          :class="{ invalid: manualMode && maxSize !== '' && !sizeValid }"
          type="text"
          spellcheck="false"
          autocomplete="off"
          :placeholder="sizePlaceholder"
          :disabled="running || !manualMode"
        />
        <span class="hint num">{{ sizeHuman }}</span>
      </div>

      <!-- compression level (automatic only) -->
      <div class="row" :class="{ dimmed: manualMode }">
        <span class="label">سطح فشرده‌سازی:</span>
        <div class="choices">
          <label class="radio">
            <input v-model="level" type="radio" value="low" :disabled="running || manualMode" />
            <span>کم</span>
          </label>
          <label class="radio">
            <input v-model="level" type="radio" value="medium" :disabled="running || manualMode" />
            <span>متوسط</span>
          </label>
          <label class="radio">
            <input v-model="level" type="radio" value="high" :disabled="running || manualMode" />
            <span>زیاد</span>
          </label>
          <label class="radio">
            <input v-model="level" type="radio" value="extreme" :disabled="running || manualMode" />
            <span>خیلی زیاد</span>
          </label>
        </div>
      </div>

      <div class="divider"></div>

      <!-- destination -->
      <div class="row">
        <span class="label">محل ذخیره:</span>
        <div class="choices">
          <label class="radio">
            <input v-model="outputMode" type="radio" value="replace" :disabled="running" />
            <span>جاگزین کردن</span>
          </label>
          <label class="radio">
            <input v-model="outputMode" type="radio" value="copy" :disabled="running" />
            <span>کپی به آدرس:</span>
          </label>
        </div>
      </div>

      <div class="row" :class="{ dimmed: !copyMode }">
        <label class="label" for="out">پوشه خروجی:</label>
        <input
          id="out"
          v-model="outputDir"
          class="field"
          type="text"
          spellcheck="false"
          autocomplete="off"
          :disabled="running || !copyMode"
          @change="commitTypedOutput"
        />
        <button class="btn" :disabled="running || !copyMode" @click="browse('output')">
          جستجو
        </button>
      </div>

      <div class="row" :class="{ dimmed: !copyMode }">
        <span class="label">در صورت موجود بودن فایل خروجی:</span>
        <div class="choices">
          <label class="radio">
            <input
              v-model="existing"
              type="radio"
              value="overwrite"
              :disabled="running || !copyMode"
            />
            <span>جاگزین کردن</span>
          </label>
          <label class="radio">
            <input v-model="existing" type="radio" value="skip" :disabled="running || !copyMode" />
            <span>پرش</span>
          </label>
        </div>
      </div>

      <div class="divider"></div>

      <!-- speed -->
      <div class="row">
        <span class="label">نوع خروجی:</span>
        <div class="choices">
          <label class="radio">
            <input v-model="speed" type="radio" value="optimal" :disabled="running" />
            <span>بهینه</span>
          </label>
          <label class="radio">
            <input v-model="speed" type="radio" value="fast" :disabled="running" />
            <span>سریع</span>
          </label>
        </div>
      </div>

      <!-- progress + action -->
      <div class="row actions">
        <div class="progress-wrap">
          <div class="progress">
            <div class="bar" :style="{ width: percent + '%' }"></div>
          </div>
          <div class="progress-meta">
            <span v-if="running && fileCount">{{ fileIndex + 1 }} / {{ fileCount }}</span>
            <span v-if="running" class="pct">{{ percent }}٪</span>
          </div>
        </div>
        <button
          class="btn primary"
          :class="{ cancel: running }"
          :disabled="!running && !canStart"
          @click="toggleStart"
        >
          {{ running ? 'لغو' : 'شروع' }}
        </button>
      </div>

      <div v-if="running && currentFile" class="current-wrap">
        <p class="current" :title="currentFile">{{ currentFile }}</p>
        <p v-if="detailLine" class="detail">{{ detailLine }}</p>
      </div>

      <!-- log: every message is Persian, so the pane stays right-to-left (from
           <html dir>) even when a line starts with a Latin word such as
           "ffmpeg"; paths inside the lines are isolated by LogText. -->
      <div ref="logEl" class="log">
        <div v-for="line in logLines" :key="line.id" class="log-line"><LogText :entry="line" /></div>
      </div>

      <div v-if="dropActive" class="drop-hint">فایل‌ها یا پوشه‌ها را اینجا رها کنید</div>
    </main>

    <footer class="dev">developed by Sina0</footer>
  </div>
</template>
