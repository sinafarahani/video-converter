# Developing Video Converter

C++20 with CMake. [CEF](https://bitbucket.org/chromiumembedded/cef) (Chromium
Embedded Framework) provides the window, Vue 3 the interface, and FFmpeg does
the encoding as a separate program.

## Layout

```
engine/      conversion core -- no CEF, no UI, no Windows-only interfaces
app/         CEF shell: window, JS bridge, asset serving, platform entry points
  mac/       Info.plist templates for the app and its helper bundles
  linux/     .desktop entry and per-user install.sh
  res/       icons (.ico / .icns / .png) and the Windows resource script
ui/          Vue 3 frontend; `npm run build` emits ui/dist
tools/       convctl, a command-line harness for the engine (not shipped)
installer/   NSIS script for the Windows installer (per-user, no elevation)
scripts/     build.ps1 (Windows) and fetch-ffmpeg.sh (all platforms)
cmake/       DownloadCEF.cmake -- fetches the pinned CEF on first configure
```

## Prerequisites

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | Visual Studio 2022 or 2026 with *Desktop development with C++* (Build Tools is enough) | GCC 12+ or Clang 15+ | Xcode command-line tools |
| CMake 3.24+ and Ninja | included with Visual Studio | `apt install cmake ninja-build` | `brew install cmake ninja` |
| Node.js 18+ | yes | yes | yes |
| Other | Git Bash (for `fetch-ffmpeg.sh`) | `libx11-dev` | — |

Plus [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` pointing
at it and three packages installed globally:

```sh
git clone https://github.com/microsoft/vcpkg && ./vcpkg/bootstrap-vcpkg.sh   # .bat on Windows
export VCPKG_ROOT=$PWD/vcpkg
$VCPKG_ROOT/vcpkg install nlohmann-json spdlog fmt --triplet x64-linux
#   Windows: x64-windows-static   macOS: arm64-osx or x64-osx
```

CEF (~160 MB) is downloaded into `third_party/cef` on the first configure.

## Building

Fetch the FFmpeg build the app ships with, then configure and build with the
preset for your platform:

```sh
scripts/fetch-ffmpeg.sh linux-x64          # windows-x64 | linux-x64 | macos-arm64 | macos-x64
cmake --preset linux-x64                   # windows-x64 | linux-x64 | macos-arm64 | macos-x64
cmake --build --preset linux-x64-release
```

The runnable app is staged in `build/<preset>/app/Release/` (on macOS,
`converter.app` inside it).

### Windows specifics

`scripts\build.ps1` finds whichever Visual Studio edition is installed, enters
its developer environment and runs the preset:

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
.\scripts\build.ps1                          # Ninja, Release
.\scripts\build.ps1 -Preset windows-x64-vs   # generate a Visual Studio solution
.\scripts\build.ps1 -EngineOnly              # engine + convctl only
.\scripts\build.ps1 -Clean                   # wipe the build dir first
```

`windows-x64-vs` writes `build/windows-x64-vs/converter.slnx` (VS 2026 uses the
XML `.slnx` format); open it, set **converter** as the startup project, F5.

**Why the script reads `VCPKG_ROOT` early:** Visual Studio's developer shell
overwrites `VCPKG_ROOT` with the vcpkg bundled inside VS, which only supports
manifest mode and cannot see globally installed packages. The script captures
it first and puts it back.

### Machine-specific paths

Put them in a `CMakeUserPresets.json` (git-ignored) rather than editing the
shared presets. `build.ps1` picks up a preset named `local-windows`
automatically:

```json
{
  "version": 3,
  "configurePresets": [{
    "name": "local-windows",
    "inherits": "windows-x64",
    "cacheVariables": {
      "CMAKE_TOOLCHAIN_FILE": "D:/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "CEF_ROOT": "D:/deps/cef/cef_binary_151.3.24+g2384915+chromium-151.0.7922.174_windows64_minimal"
    }
  }],
  "buildPresets": [{ "name": "local-windows-release", "configurePreset": "local-windows", "configuration": "Release" }]
}
```

### Engine only

The engine does not need CEF, so it can be built and tested in seconds:

```sh
cmake -S . -B build/engine-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DCONVERTER_ENGINE_ONLY=ON \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build/engine-only
```

## convctl

A harness for the parts that are hard to get right, runnable without the UI.
Copy ffmpeg next to it first.

```
convctl gpu                              adapters, encoder choice, measured fps
convctl bench <encoder> [ffmpeg args]    steady-state throughput of one encoder
convctl quote                            Windows argv quoting self-test
convctl size [values...]                 size parsing (built-in table if none)
convctl probe <file>                     streams, per-track loudness, stereo pairs
convctl run <dir> [--out <dir>] [--size 1GB | --level medium]
           [--fast] [--skip] [--cpu] [--cancel-after <s>]
```

## Releasing

1. Bump `VERSION` in the top-level `CMakeLists.txt`.
2. Add a `## [x.y.z] - date` section to `CHANGELOG.md` — the release notes are
   taken from it.
3. Commit, then tag and push:

   ```sh
   git tag v2.0.1 && git push origin v2.0.1
   ```

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds all
four packages, runs the engine tests on each, and publishes the release with a
`SHA256SUMS.txt`. Running the workflow by hand from the Actions tab builds the
packages without publishing.

| Platform | Runner | Package |
|---|---|---|
| Windows x64 | `windows-latest` | NSIS installer + portable zip |
| Linux x64 | `ubuntu-22.04` | `.tar.gz` with `install.sh` |
| macOS Apple Silicon | `macos-latest` | `.dmg` |
| macOS Intel | `macos-15-intel` (until August 2027) | `.dmg` |

The Linux build runs on an older Ubuntu on purpose: the binary requires the
glibc it was built against.

**Signing.** Packages are not signed with paid certificates. macOS bundles are
ad-hoc signed inside-out in CI, which Apple Silicon requires just to run code;
without a Developer ID and notarisation, users confirm the app once in System
Settings. Windows users see a SmartScreen prompt.

## Design notes

### Why ffmpeg is a separate program

libx264 and libx265 are GPL. Linking them would oblige the whole application to
be GPL; running `ffmpeg` as a separate program over argv and pipes keeps that
boundary intact, which is why the engine shells out instead of calling
libavcodec. Linking would not have paid off technically either: similar size,
thousands of lines of new code, and it would not even remove the two-pass stats
file, because libx264 does its own file I/O.

`scripts/fetch-ffmpeg.sh` pins the builds that ship (FFmpeg 9.0 from
[BtbN](https://github.com/BtbN/FFmpeg-Builds) on Windows and Linux,
[martin-riedl.de](https://ffmpeg.martin-riedl.de) on macOS), verifies their
checksums and writes `LICENSE.txt` and `FFMPEG-SOURCE-OFFER.txt` beside them.
The build refuses to package without both.

### The process layer

`engine/src/process_win.cpp` and `process_posix.cpp` drive child processes
directly: real pipes, a reader thread per stream on Windows / `poll` on POSIX,
and exit detected on the process handle. On Windows the child is placed in a
kill-on-close job object; on Linux it gets `PR_SET_PDEATHSIG`; so ffmpeg never
outlives the app.

### Encoder selection

`choose_encoder()` enumerates adapters (DXGI on Windows; elsewhere it probes
the hardware encoders ffmpeg reports), filters out ineligible ones, and
benchmarks each candidate against libx265 by sampling ffmpeg's own frame
counter — steady-state throughput, with process and driver start-up excluded by
construction. A hardware encoder must be 2× faster to win. Results are cached
per adapter and ffmpeg version; failures are retried after 24 hours.

### Audio track selection

Every audio stream is measured with `volumedetect` over three 30-second
windows. A track is kept if it is above −50 dB mean (or −35 dB peak) and within
20 dB of the loudest; if none qualify, the loudest is kept anyway. Adjacent mono
tracks that both qualify and sit within 8 dB of each other are joined into one
stereo stream — aligned pairs (1-2, 3-4, …) first, following EBU R48. The
thresholds live in `AudioDetectionConfig` in `engine/include/conv/types.hpp`.

## Lessons from the first field test

**"RTX 3050 rejected as too slow."** Two causes behind one misleading message.
The bundled ffmpeg needs a recent NVIDIA driver, and on an older one
`hevc_nvenc` fails to open — which the old code reported as "too slow". And the
old benchmark timed whole 3-second runs, where 44% of the NVENC time was CUDA
start-up; it measured 5× where the true figure was 9×. Failures are now
reported as failures, and the benchmark measures steady state.

**"Stuck at 0% for three days."** The first process layer used reproc, which on
Windows emulates pipes with loopback TCP sockets. After a transient socket
error the reader gave up while ffmpeg kept writing; the pipe filled, ffmpeg
blocked, and the app sat idle indefinitely. Replaced by the direct
implementation above, plus live progress detail, a stall watchdog and full
command logging so "slow" and "stuck" can always be told apart.

**MXF audio.** The first real file had 8 mono tracks with sound on 2 — one
stereo pair stored as discrete L/R, the normal broadcast layout — which is why
stereo pairing exists.

## Things that will bite you

- `SET_CEF_TARGET_OUT_DIR()` must come **before** `add_executable`: it sets
  `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, which only affects later targets.
- MSVC needs `/EHsc`. CEF compiles with `_HAS_EXCEPTIONS=0`; we remove that
  define, and without `/EHsc` the STL assumes exceptions while the compiler
  emits no unwind code.
- Never `AppendSwitchWithValue("disable-features", ...)` — it replaces CEF's own
  feature list and CEF crashes at start-up.
- `file(GLOB_RECURSE ... index.html)` also matches `ui/dist/index.html`, the UI
  build's own output, which creates a dependency cycle.
- On macOS, `MACOSX_BUNDLE_INFO_PLIST` is expanded with global variables at the
  end of the directory, so each bundle's plist is written out by hand
  (`converter_write_plist`); executables must live in `Contents/MacOS` or code
  signing rejects the bundle; and the `NSApplication` must implement
  `CefAppProtocol` or CEF aborts.
- `configure_file` does not understand generator expressions; never give it a
  path under `CEF_TARGET_OUT_DIR`.

## Platform status

| | Windows | Linux | macOS |
|---|---|---|---|
| Entry point | `main_win.cc` | `main_posix.cc` | `main_mac.mm` + `main_mac_helper.cc` |
| Paths | `platform_win.cc` | `platform_posix.cc` | `platform_posix.cc` |
| GPU discovery | DXGI + benchmark | encoder probe + benchmark | encoder probe + benchmark |
| Package | NSIS installer, portable zip | `.tar.gz` + `install.sh` | `.dmg` |
