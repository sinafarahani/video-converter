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
cmake/       DownloadCEF.cmake -- fetches the pinned CEF on first configure;
             CheckMediaExtensions.cmake -- keeps the OS file registrations in
             step with the engine's extension list
```

## Prerequisites

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | Visual Studio 2022 or 2026 with *Desktop development with C++* (Build Tools is enough) | GCC 12+ or Clang 15+ | Xcode command-line tools |
| CMake 3.24+ and Ninja | included with Visual Studio | `apt install cmake ninja-build` | `brew install cmake ninja` |
| Node.js 22+ (CI and releases use 26) | yes | yes | yes |
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
convctl plan <paths...> [--out <dir>] [--replace]
                                         what a run would do, without ffmpeg
convctl run <paths...> [--out <dir>] [--replace] [--size 1GB | --level medium]
            [--fast] [--skip] [--cpu] [--cancel-after <s>]
```

`<paths...>` is any mix of folders and audio/video files, in the order the app
would receive them, and flags can go anywhere; an unknown flag is an error.
Without `--out` (or with `--replace`) files are replaced in place, as in the
app's Replace mode. On Windows convctl takes its arguments as UTF-16 (`wmain`),
so Persian paths work from any console.

`plan` prints what `run` would do with the same arguments. It calls the
engine's own `plan_inputs`, so this is exactly what the app would do:

```
convctl plan D:\Day1 D:\Day2\Day1 D:\notes.txt --out D:\out

plan: 3 media file(s)
  D:\Day1\a.mkv -> D:\out\Day1\a.mp4
  D:\Day1\a.mov -> D:\out\Day1\a (2).mp4 [renamed]
  D:\Day2\Day1\b.mp4 -> D:\out\Day1 (2)\b.mp4
missing: 0
not media: 1
  D:\notes.txt
duplicates: 0
ignored in folders: 0
excluded (inside output folder): 0
```

The release workflow greps these lines, so keep their shape.

## Releasing

1. Bump `VERSION` in the top-level `CMakeLists.txt`.
2. Add a `## [x.y.z] - date` section to `CHANGELOG.md` — the release notes are
   taken from it.
3. Commit, then tag and push:

   ```sh
   git tag v2.1.0 && git push origin v2.1.0
   ```

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds all
four packages, runs the engine tests on each, checks on Linux that the app
makes no network connections, lints the macOS Info.plist, and publishes the
release with a `SHA256SUMS.txt`. Running the workflow by hand from the Actions tab builds the
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

### Inputs: folders and files

`Settings::inputs` is a list of folders and files, in the order given.
`plan_inputs()` in `engine/src/job.cpp` turns it into the files to process
before anything else happens -- before the encoder benchmark, so a run with
nothing to do costs nothing -- and decides every output name up front:

- Folders are walked recursively and sorted, so the order, and with it any
  ` (2)` names, is the same on every OS. Links to folders are not followed.
  A subfolder that cannot be listed is skipped, named in the run log and
  counted in one line in the UI; the walk carries on with the rest.
  Non-media files inside folders are counted and left alone; a non-media file
  given directly is reported, and so is an engine temp file (`*_tmp<hex>`).
- Files are keyed by canonical path (`path_key`: case-folded with NTFS's rules
  on Windows, ASCII only on macOS), so a file reached twice -- picked on its
  own and inside a picked folder -- is processed once, with the folder's
  layout. A symlink given directly is keyed where it sits, as a folder walk
  would key it.
- An output name is taken if an earlier file of this run got it, if it is an
  input of this run (in Copy mode its own input too: an output folder equal to
  the input folder never loses an original), or (Replace mode only) if an
  unrelated file already sits there. Taken names get ` (2)`, ` (3)`, and so
  on. Files from earlier runs in the output folder are still left to the
  Overwrite/Skip setting; when several inputs share an output name, Skip says
  in the UI that the existing file may have come from another of them.
- `finalize()` checks the disk once more, by file identity (`file_id`), for
  what keys cannot see (non-ASCII case on macOS, Unicode normalisation, hard
  links): an output never replaces an input or an earlier result of this run,
  and in Replace mode never replaces an existing file at all. It takes the
  next free ` (n)` name instead.
- In Copy mode a lone input folder maps straight onto the output folder, as
  it always did. With several inputs each folder gets `out/<its name>/` and
  loose files go to `out/`; a folder that contributes no files of its own
  takes no name. An output folder inside an input folder is skipped while
  walking it, and so is any `out/<name>/` that lies inside an input folder
  (the output folder being that input folder, say), or every run would
  convert the previous run's results again, one level deeper.

### Where inputs come from

Besides the page's own pickers, four routes deliver paths. All of them end in
`Bridge::DeliverInputs`, which keeps existing folders and existing files with
a media extension and sends the page an `inputs` event:

| Route | Code | `source` |
|---|---|---|
| Command line at first launch (Explorer "Open with", Linux `%F`, a terminal) | `OnContextInitialized` via `paths_from_command_line` | `cmdline` |
| A second launch while the app runs | `OnAlreadyRunningAppRelaunch` | `relaunch` |
| Finder "Open With" and Dock drops (macOS) | `application:openURLs:` in `main_mac.mm` | `openWith` |
| Files dropped on the window | `OnDragEnter` stores the paths; the page collects them on drop with `takeDroppedPaths` | `drop` |

CEF's process singleton, keyed on `root_cache_path`, forwards a second
launch's command line to the running process and makes the new one exit with
code 24 (`CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED`), which the entry
points turn into 0. Explorer starts one process per selected file, so
deliveries less than 1.5 s apart are appended to one list instead of each
replacing the last.

The operating systems learn about the app from the installer (a ProgID listed
under each extension's `OpenWithProgids`, never an extension's default; a
folder verb; a Send To shortcut), `Info.plist` (document types) and the
`.desktop` file (MIME types). The extension lists live in
`engine/src/util.cpp`; `cmake/CheckMediaExtensions.cmake` fails the configure
when a registration falls out of step with them.

### Offline

The app makes no network contact of its own. The layers overlap on purpose;
all of them are tables at the top of `app/src/app.cc`, each entry with the
reason it exists.

- **Switches**, active from process start: `disable-background-networking`,
  `disable-component-update`, `disable-domain-reliability`, `disable-sync`,
  `no-pings`, `no-proxy-server` and a few more.
- **Features**, merged into `--disable-features` (see the gotcha below):
  network time, which `disable-background-networking` does not cover; AI Mode
  eligibility; the Optimization Guide; Media Router / Cast discovery; Autofill
  server queries.
- **Preferences**, set in `OnContextInitialized` before the browser exists:
  spell-check and its dictionary download, Safe Browsing, suggestions,
  translate, network prediction and the like on the profile (saved in
  `cef-cache/Default/Preferences`, and set again on every launch so that a
  profile from an older version is corrected too); network time, component
  updates and DNS-over-HTTPS in `Local State`. A preference Chromium rejects
  is logged to `cef.log` and skipped.
- **`host-resolver-rules=MAP * ^NOTFOUND`**: every hostname lookup inside
  Chromium fails. The page comes from `app://` through a scheme handler and
  needs no DNS, so this catches whatever the lists miss. It cannot stop
  IP-literal connections or LAN multicast; the features do that. Caret, not
  tilde: Chromium 151 only knows `^NOTFOUND`.
- **No Chrome UI**: the app runs the Chrome runtime with an Alloy-style
  window and browser (`BrowserViewDelegate` and `WindowDelegate` in `app.cc`),
  so there are no Chrome shortcuts, menus or tabs. The context menu keeps only
  edit items (`OnBeforeContextMenu`), so no "Search Google for", Translate or
  Lens. `OnChromeCommand`'s deny-list and `OnOpenURLFromTab` are guards
  behind that. `GetDefaultClient` returns a `StrayBrowserClient` that closes any
  window Chrome opens by itself, and the macOS Dock menu offers no New Window.
  `OnBeforeBrowse` cancels every navigation outside `app://converter/`,
  `OnBeforePopup` cancels every popup, and the relaunch handler always returns
  true. Chrome-style windows (a New Tab Page opened by a relaunch or a
  shortcut) caused several of the Google contacts found in a 2.0 profile.

ffmpeg is a separate process and is only ever given existing local files.

To check on Linux, with a fresh profile:

```sh
export XDG_DATA_HOME=$(mktemp -d)                  # user_data_dir() follows it
strace -f -e trace=connect,sendto,sendmsg,sendmmsg -o /tmp/vc.strace ./converter
grep -E 'sa_family=AF_INET6?\b' /tmp/vc.strace     # expect nothing
```

The sends are traced because mDNS and SSDP multicast go out with `sendto()` on
an unconnected socket and never call `connect()`. The grep matches only a
destination address; a bare `AF_INET` would also match the netlink requests
strace decodes. Media Router discovery starts on demand, so an idle run shows
only that nothing happens at startup; the Media Router features stay the real
guard against it.

Optionally, run with no network at all; the app must work the same:

```sh
unshare -rn ./converter
```

That needs unprivileged user namespaces, which Ubuntu 23.10 and later block
through AppArmor. Allow them for the session with
`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`, and set it
back to `1` afterwards.

The release workflow runs the strace check for 30 s under `xvfb-run`; it does
not use `unshare`, for the reason above. On
Windows, Process Monitor filtered to `converter.exe`, with Operation beginning
with TCP or UDP, should show nothing. Move
`%LOCALAPPDATA%\VideoConverter\cef-cache` aside first: a profile from an older
version may already hold Google state. On any platform,
`--log-net-log=vc.json --net-log-capture-mode=Everything` gives Chromium's own
view.

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
- Never write `--disable-features` over its existing value. CEF has already put
  its own list there (GlicActorUi, LensOverlay, ...) by the time
  `OnBeforeCommandLineProcessing` runs, and appending the switch again
  replaces the value outright -- CEF's list is lost and CEF crashes at
  start-up. Read it, add what is missing, `RemoveSwitch`, append:
  `merge_disabled_features` in `app.cc`.
- `OnAlreadyRunningAppRelaunch` must return true, always -- with no paths, and
  before the window exists too. `false` makes Chrome open a default-styled
  window of its own in the running process: unmanaged, able to browse, with a
  New Tab Page that talks to Google and a "Restore pages?" bubble.
- The page's `drop` event only carries file names. The full paths exist only
  in `CefDragHandler::OnDragEnter`, which stores them for `takeDroppedPaths`
  and must return false, or the page never sees the drag. CEF calls
  `OnDragEnter` only for Alloy-style browsers (`AlloyBrowserHostImpl`); a
  Chrome-style browser's drags never reach CEF. That is why `app.cc`'s
  `BrowserViewDelegate::GetBrowserRuntimeStyle` and
  `WindowDelegate::GetWindowRuntimeStyle` return `CEF_RUNTIME_STYLE_ALLOY`,
  and they must stay that way. The page must
  `preventDefault()` both `dragover` and `drop`, or Chromium navigates to the
  file (which `OnBeforeBrowse` then cancels).
- The file picker passes no `accept_filters` on purpose. CEF 151 drops the
  documented `"description|.ext;.ext"` form, a `;`-joined list becomes one
  broken pattern, and plain extensions (`".mp4"`, `".mkv"`, ...) become one
  dialog entry per extension with the first pre-selected -- the picker would
  open showing `.mp4` files only. Showing every file and rejecting non-media
  picks in the reply is the honest option. On Linux the dialog comes from the
  desktop portal over the session bus; with no session bus (bare Xvfb tests)
  `RunFileDialog` shows nothing, in either runtime style.
- `--password-store=basic` must stay: without it Chromium on Linux asks the
  desktop keyring for a key at start-up and pops up "Choose password for new
  keyring" wherever none is unlocked.
- Events sent before the page subscribes are dropped by `Emit`. Command-line
  and relaunch paths arrive before Vue has mounted, so `inputs` events go
  through `EmitOrBuffer` and are flushed after `subscribe`, from a posted task:
  never call `Success()` on the subscribe query from inside its own `OnQuery`.
- Pass a navigation to the message router's `OnBeforeBrowse` only if it goes
  ahead. Told about a cancelled one, the router drops the page's event
  subscription.
- macOS: `NSApp.delegate` can only be replaced after `CefInitialize`, because
  Chrome checks it is nil and installs its own `AppController` during start-up.
  Ours forwards every selector it does not implement to that one. Without it,
  Chrome's delegate would open Finder's files as `file://` tabs.
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
| Opened files | `launch_paths.cc` + relaunch | `launch_paths.cc` + relaunch | `application:openURLs:` + `launch_paths.cc` |
| Open with | installer: OpenWithProgids, folder verb, Send To | `.desktop` MimeType, `%F` | Info.plist document types |
| GPU discovery | DXGI + benchmark | encoder probe + benchmark | encoder probe + benchmark |
| Package | NSIS installer, portable zip | `.tar.gz` + `install.sh` | `.dmg` |
