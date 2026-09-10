# Changelog

All notable changes to Video Converter. Versions follow [Semantic Versioning](https://semver.org/).

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

[2.0.1]: https://github.com/sinafarahani/video-converter/releases/tag/v2.0.1
