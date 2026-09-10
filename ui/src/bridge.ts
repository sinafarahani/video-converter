// The single point of contact with the C++ side.
//
// CEF injects window.cefQuery into every frame. Everything below wraps that in
// promises and typed events so the components never touch it directly.

interface CefQueryOptions {
  request: string
  persistent: boolean
  onSuccess: (response: string) => void
  onFailure: (errorCode: number, errorMessage: string) => void
}

declare global {
  interface Window {
    cefQuery?: (options: CefQueryOptions) => number
    cefQueryCancel?: (requestId: number) => void
  }
}

export const isNative = typeof window.cefQuery === 'function'

/** One-shot request/response. Rejects if the C++ side reports a failure. */
export function invoke<T = unknown>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
  if (!isNative) return mockInvoke<T>(cmd, args)

  return new Promise<T>((resolve, reject) => {
    window.cefQuery!({
      request: JSON.stringify({ cmd, args }),
      persistent: false,
      onSuccess: (response) => {
        try {
          resolve(response ? (JSON.parse(response) as T) : ({} as T))
        } catch (e) {
          reject(new Error(`bad response from ${cmd}: ${String(e)}`))
        }
      },
      onFailure: (code, message) => reject(new Error(`${cmd} failed (${code}): ${message}`)),
    })
  })
}

// ---------------------------------------------------------------------------
// Events pushed from C++
// ---------------------------------------------------------------------------

export interface ProgressEvent {
  type: 'progress'
  fraction: number      // 0..1 across the whole run
  fileFraction: number  // 0..1 within the current file
  index: number
  count: number
  file: string
  phase: string         // probe | analyze | pass1 | pass2 | encode | remux | audio
  pass: number
  passCount: number
  fps: number
  speed: number         // multiple of realtime
  outTime: number       // seconds produced
  duration: number      // seconds of input
  elapsed: number       // seconds since this file started
  eta: number           // seconds; -1 when unknown
}

/**
 * Paths handed to the app by the OS: command line, a second launch, "Open
 * with", or a drop. C++ has already sorted them: `paths` are existing folders
 * and audio/video files, `rejected` is everything else.
 */
export interface InputsEvent {
  type: 'inputs'
  paths: string[]
  rejected: string[]
  source: 'cmdline' | 'relaunch' | 'openWith' | 'drop'
  append: boolean       // add to the current list instead of replacing it
}

export type EngineEvent =
  // `paths`, when present, are the exact paths inside `text`; without it the
  // UI recognises the engine's messages by their wording (logText.ts).
  | { type: 'log'; text: string; paths?: string[] }
  | ProgressEvent
  | InputsEvent
  | {
      type: 'finished'
      converted: number
      remuxed: number
      skipped: number
      failed: number
      cancelled: boolean
      notMedia: number      // files given directly that are not audio/video
      missing: number       // inputs that did not exist
    }

/**
 * Opens the persistent event channel. The returned function closes it.
 *
 * This is a `persistent: true` query, which means C++ can call Success() on it
 * repeatedly -- it is the push channel, not a one-off call.
 */
export function subscribe(handler: (event: EngineEvent) => void): () => void {
  if (!isNative) return mockSubscribe(handler)

  const id = window.cefQuery!({
    request: JSON.stringify({ cmd: 'subscribe' }),
    persistent: true,
    onSuccess: (response) => {
      try {
        handler(JSON.parse(response) as EngineEvent)
      } catch {
        /* a malformed event should never kill the channel */
      }
    },
    onFailure: () => {
      /* channel closed by the browser going away */
    },
  })

  return () => window.cefQueryCancel?.(id)
}

// ---------------------------------------------------------------------------
// Browser fallback, so `npm run dev` is usable without building the C++ app
// ---------------------------------------------------------------------------

let mockHandler: ((event: EngineEvent) => void) | null = null
let mockTimer: number | null = null

function mockSubscribe(handler: (event: EngineEvent) => void): () => void {
  mockHandler = handler
  return () => {
    mockHandler = null
  }
}

function mockInvoke<T>(cmd: string, args: Record<string, unknown>): Promise<T> {
  const reply = (value: unknown) => Promise.resolve(value as T)

  switch (cmd) {
    case 'getState':
      return reply({ ffmpegFound: true, ffmpeg: '(mock)', running: false, logDir: '(mock)' })

    case 'pickFolder':
      return reply({ cancelled: false, path: 'C:\\نمونه\\ویدیوها', paths: ['C:\\نمونه\\ویدیوها'] })

    case 'pickFiles':
      return reply({
        cancelled: false,
        paths: ['C:\\نمونه\\ویدیوها\\clip.mkv', 'C:\\نمونه\\صدا\\song.mp3'],
        rejected: [],
      })

    // A browser cannot see where a dropped file lives, so any drop yields the
    // same mixed sample -- enough to exercise the list and the rejected log.
    case 'takeDroppedPaths':
      return reply({
        paths: ['C:\\نمونه\\ویدیوها', 'C:\\نمونه\\clip.mp4', 'C:\\نمونه\\song.wav'],
        rejected: ['C:\\نمونه\\notes.txt'],
      })

    case 'parseSize': {
      const text = String(args.text ?? '')
      const ok = /^\s*[\d.]+\s*(gb|mb|kb)?\s*$/i.test(text)
      return reply({ valid: ok, bytes: ok ? 1073741824 : 0, human: ok ? '1.00 GB' : '' })
    }

    case 'cancel':
      if (mockTimer) window.clearInterval(mockTimer)
      mockTimer = null
      mockHandler?.({ type: 'log', text: 'عملیات لغو شد.' })
      mockHandler?.({
        type: 'finished',
        converted: 0,
        remuxed: 0,
        skipped: 0,
        failed: 0,
        cancelled: true,
        notMedia: 0,
        missing: 0,
      })
      return reply({})

    case 'start': {
      let n = 0
      mockHandler?.({ type: 'log', text: 'در حال شروع عملیات لطفا صبر کنید...' })
      mockTimer = window.setInterval(() => {
        n += 0.04
        const fileFrac = (n * 3) % 1
        mockHandler?.({
          type: 'progress',
          fraction: Math.min(n, 1),
          fileFraction: fileFrac,
          index: Math.floor(n * 3),
          count: 3,
          file: 'C:\\نمونه\\ویدیوها\\clip.mkv',
          phase: fileFrac < 0.5 ? 'pass1' : 'pass2',
          pass: fileFrac < 0.5 ? 1 : 2,
          passCount: 2,
          fps: 1.4 + Math.random() * 0.2,
          speed: 0.028,
          outTime: fileFrac * 3600,
          duration: 3600,
          elapsed: n * 600,
          eta: (1 - fileFrac) * 3600 / 0.028,
        })
        if (n >= 1) {
          if (mockTimer) window.clearInterval(mockTimer)
          mockTimer = null
          mockHandler?.({ type: 'log', text: 'عملیات به پایان رسید.' })
          mockHandler?.({
            type: 'finished',
            converted: 3,
            remuxed: 0,
            skipped: 0,
            failed: 0,
            cancelled: false,
            notMedia: 0,
            missing: 0,
          })
        }
      }, 200)
      return reply({ started: true })
    }

    default:
      return reply({})
  }
}
