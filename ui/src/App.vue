<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { invoke, subscribe, type EngineEvent } from './bridge'

type OutputMode = 'replace' | 'copy'
type Existing = 'overwrite' | 'skip'
type Speed = 'optimal' | 'fast'
type SizeMode = 'manual' | 'auto'
type Level = 'low' | 'medium' | 'high' | 'extreme'

const inputDir = ref('')
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

const logLines = ref<string[]>([])
const logEl = ref<HTMLElement | null>(null)

// The Java version let its log pane grow without limit for the whole session.
// A large batch produces thousands of lines, and every one of them stays in the
// DOM; capping keeps scrolling smooth without losing anything that matters.
const MAX_LOG_LINES = 2000

function log(text: string) {
  logLines.value.push(text)
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

const canStart = computed(() => {
  if (!inputDir.value) return false
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

async function browse(which: 'input' | 'output') {
  try {
    const r = await invoke<{ cancelled: boolean; path: string }>('pickFolder', {
      title: which === 'input' ? 'پوشه ورودی را انتخاب کنید' : 'پوشه خروجی را انتخاب کنید',
      initial: which === 'input' ? inputDir.value : outputDir.value,
    })
    if (r.cancelled || !r.path) return
    if (which === 'input') inputDir.value = r.path
    else outputDir.value = r.path
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
    if (!inputDir.value) log('فیلد پوشه ورودی نمیتواند خالی باشد')
    else if (copyMode.value && !outputDir.value) log('فیلد پوشه خروجی نمیتواند خالی باشد')
    else if (manualMode.value) log('حداکثر اندازه خروجی نامعتبر است.')
    return
  }

  progress.value = 0
  running.value = true

  try {
    const r = await invoke<{ started: boolean }>('start', {
      inputDir: inputDir.value,
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

function onEvent(ev: EngineEvent) {
  switch (ev.type) {
    case 'log':
      log(ev.text)
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

onMounted(async () => {
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

onUnmounted(() => unsubscribe?.())
</script>

<template>
  <div class="shell">
    <main class="card">
      <!-- input directory -->
      <div class="row">
        <label class="label" for="in">پوشه ورودی:</label>
        <input
          id="in"
          v-model="inputDir"
          class="field"
          type="text"
          placeholder="یک پوشه انتخاب کنید"
          :disabled="running"
        />
        <button class="btn" :disabled="running" @click="browse('input')">جستجو</button>
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
          :disabled="running || !copyMode"
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

      <!-- log -->
      <div ref="logEl" class="log" dir="auto">
        <div v-for="(line, i) in logLines" :key="i" class="log-line">{{ line }}</div>
      </div>
    </main>

    <footer class="dev">developed by Sina0</footer>
  </div>
</template>
