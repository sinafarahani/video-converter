# Changelog

All notable changes to Video Converter. Versions follow [Semantic Versioning](https://semver.org/).

## [2.1.0] - 2026-09-11

### Added
- **Files as well as folders.** The input can be a folder, as before, or one
  or more video and audio files, or a mix of both. A new **انتخاب فایل**
  (Choose files) button sits next to **انتخاب پوشه** (Choose folder).
- **Drag and drop.** Drop files and folders anywhere on the window.
- **Open with** on every platform: Windows (through the installer), macOS
  Finder and the Linux desktop (after `install.sh`) list the app for every
  supported format. It is
  offered as a choice, never made the default app.
- **Windows folder menu and Send To.** Right-click a folder and choose
  **تبدیل با مبدل ویدیو** (Convert with Video Converter). On Windows 11 it is
  under *Show more options*. Or send any mix of files and folders to the app
  with *Send to*.
- **One window.** Opening files while the app is already running hands them
  to the open window instead of starting a second copy. Several files opened
  together arrive as one list. Files that arrive during a conversion are
  picked up when it finishes.
- **Works offline.** The app makes no network connections of its own:
  Chromium's background services (time sync, component updates,
  spell-check dictionaries, Safe Browsing, suggestions and the like) are
  switched off, and the window can only show the app itself.
- A file given directly that is neither video nor audio is left untouched,
  with a line in the log saying so. Inputs that no longer exist are reported
  too.

### Fixed
- Replace mode: converting `clip.avi` deleted an unrelated `clip.mp4` in the
  same folder to make room for its result. The result is now saved as
  `clip (2).mp4`.
- Copy mode: two files that map to the same output name (`a.avi` and `a.mkv`
  both become `a.mp4`) overwrote each other. The second is now `a (2).mp4`.
- Copy mode with the output folder inside the input folder: the output folder
  is no longer read as input, so a second run does not convert the first
  run's results again.
- Copy mode with the output folder set to the input folder: an `.mp4` or
  `.mp3` input was replaced by its own result, deleting the original. The
  result is now saved next to it as `name (2).mp4`.
- A subfolder that could not be read (no permission, a broken link) silently
  ended the folder walk, so every file after it was skipped. The walk now
  carries on, and the log says which folders could not be read.
- Starting the app while it was already running opened a stray browser window
  ("Index of …", with a "Restore pages?" prompt) in the running copy.
- Linux and macOS: a path in the log that starts with `/` was shown with the
  slash moved to the end of the line.

### Changed
- Copy mode with several inputs: each input folder gets its own subfolder in
  the output folder (`out\Day1\…`, `out\Day2\…`, subfolders preserved; a
  second folder with the same name becomes `Day1 (2)`), and single files go
  straight into the output folder. A single input folder keeps the old layout,
  with its contents directly in the output folder.
- The file counter ("3 / 10") counts only video and audio files.
- Files inside a folder are processed in name order on every system.
- The right-click menu offers only editing commands (cut, copy, paste, select
  all); the browser's own entries such as *Search Google for…* are gone.

## [2.0.1] - 2026-09-10

The first public release, and a complete rewrite of the original JavaFX
application (1.3) in C++ with a Chromium-based interface.

### Added
- **Automatic mode.** Besides a fixed target size, files can be compressed
  at a constant quality, with four levels: Low, Medium, High and Extreme.
- **GPU encoding.** NVIDIA (NVENC), Intel Arc (Quick Sync), AMD (AMF) and Apple
  VideoToolbox are used automatically when the GPU is measured to be at least
  twice as fast as the CPU. Intel HD/UHD integrated graphics are excluded.
- **H.265 (HEVC) output** for 25-50% smaller files at the same quality,
  tagged `hvc1` so Apple devices and most players accept it.
- **Smart audio selection.** Every audio track is measured and only tracks
  with real content are kept. Silent or noise-only microphone tracks — common
  in studio recordings — are dropped. Two adjacent mono tracks that form a
  stereo pair (the usual broadcast MXF layout) are joined into one stereo track.
- **Files already smaller than the target are copied as-is** instead of being
  re-encoded larger.
- **Live progress**: phase, per-file percentage, frames per second, speed and
  an ETA. A warning appears in the log if an encode stops making progress.
- macOS (Apple Silicon and Intel) and Linux versions.
- Many more input formats: MXF, WebM, 3GP, MPEG-TS, M2TS, OGV, Opus, WMA, APE
  and others.
- Persian and Arabic-Indic digits are accepted in the size field.

### Fixed
- Replace mode left the original file behind whenever the extension changed
  (`clip.avi` produced `clip.mp4` but `clip.avi` stayed).
- Output files overshot the requested size because the audio track was not
  accounted for.
- An empty or invalid size silently stopped the conversion and left the window
  stuck in its running state.
- Two-pass statistics files were left in the working directory.
- Very short files crashed the size calculation.
- ffmpeg is bundled and found next to the app, instead of requiring a separate
  installation on the `PATH`.
- Closing or killing the app mid-conversion no longer leaves ffmpeg running or
  a half-written file behind.
- A log file is no longer created on every launch.

### Changed
- Installs per user, without administrator rights, on all platforms.
- Logs are kept in the user's data folder rather than next to the program.

[2.1.0]: https://github.com/sinafarahani/video-converter/releases/tag/v2.1.0
[2.0.1]: https://github.com/sinafarahani/video-converter/releases/tag/v2.0.1
