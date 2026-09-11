#!/usr/bin/env bash
# Fetches the FFmpeg build that ships with Video Converter for one platform,
# verifies its checksum, and writes the GPL licence and a source offer beside it.
#
#   scripts/fetch-ffmpeg.sh <platform> [dest]
#
#   platform  windows-x64 | linux-x64 | macos-arm64 | macos-x64
#   dest      default: third_party/ffmpeg
#
# Every platform ships FFmpeg 9.0.x configured with --enable-gpl
# --enable-version3 (libx264, libx265, libmp3lame), so the binaries are GPLv3.
# The app only ever runs them as separate programs; see THIRD-PARTY-NOTICES.md.
#
# Runs on Linux, macOS, and Windows under Git Bash.
set -euo pipefail

platform="${1:?usage: $0 <windows-x64|linux-x64|macos-arm64|macos-x64> [dest]}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${2:-$root/third_party/ffmpeg}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

BTBN="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest"
RIEDL="https://ffmpeg.martin-riedl.de/download/macos"
# Pinned macOS builds (FFmpeg 9.0.1). Bump both together.
RIEDL_ARM64="arm64/1787073674_9.0.1"
RIEDL_X64="amd64/1787081194_9.0.1"
REPO_URL="https://github.com/sinafarahani/video-converter"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

fetch() {
    echo "  downloading $1"
    # Fail fast and retry anything: a connection that never opens used to hang
    # for minutes (curl exit 28) and then fail the whole build. A transfer that
    # stalls below 1 KB/s for a minute counts as failed too.
    curl -fsSL --connect-timeout 20 --speed-limit 1024 --speed-time 60 \
         --retry 5 --retry-delay 5 --retry-all-errors -o "$2" "$1"
}

verify() {  # <file> <expected-sha256>
    local got
    got="$(sha256_of "$1")"
    if [ "$got" != "$2" ]; then
        echo "checksum mismatch for $(basename "$1"): got $got, expected $2" >&2
        exit 1
    fi
    echo "  sha256 ok   $(basename "$1")"
}

rm -rf "$dest"
mkdir -p "$dest"
provenance=""

case "$platform" in
    windows-x64|linux-x64)
        if [ "$platform" = windows-x64 ]; then
            # Shared build: ffmpeg.exe and ffprobe.exe share one set of DLLs,
            # which makes the pair ~130 MB smaller than two static executables.
            asset="ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip"
        else
            # Static build: no library path to manage on the user's machine.
            asset="ffmpeg-n9.0-latest-linux64-gpl-9.0.tar.xz"
        fi
        fetch "$BTBN/$asset" "$work/$asset"
        fetch "$BTBN/checksums.sha256" "$work/checksums.sha256"
        want="$(awk -v a="$asset" '$2 == a || $2 == "*" a { print $1 }' "$work/checksums.sha256")"
        if [ -z "$want" ]; then
            echo "no checksum published for $asset" >&2
            exit 1
        fi
        verify "$work/$asset" "$want"

        mkdir -p "$work/x"
        case "$asset" in
            *.zip)    (cd "$work/x" && unzip -q "../$asset") ;;
            *.tar.xz) tar -xJf "$work/$asset" -C "$work/x" ;;
        esac
        top="$(find "$work/x" -mindepth 1 -maxdepth 1 -type d | head -1)"

        if [ "$platform" = windows-x64 ]; then
            cp "$top"/bin/ffmpeg.exe "$top"/bin/ffprobe.exe "$top"/bin/*.dll "$dest/"
        else
            cp "$top"/bin/ffmpeg "$top"/bin/ffprobe "$dest/"
            chmod +x "$dest/ffmpeg" "$dest/ffprobe"
        fi
        cp "$top/LICENSE.txt" "$dest/LICENSE.txt"

        provenance="Built by:      BtbN/FFmpeg-Builds, FFmpeg 9.0 release branch, GPL variant
Archive:       $BTBN/$asset
SHA-256:       $want
Build scripts: https://github.com/BtbN/FFmpeg-Builds"
        ;;

    macos-arm64|macos-x64)
        if [ "$platform" = macos-arm64 ]; then path="$RIEDL_ARM64"; else path="$RIEDL_X64"; fi
        sums=""
        for tool in ffmpeg ffprobe; do
            fetch "$RIEDL/$path/$tool.zip" "$work/$tool.zip"
            fetch "$RIEDL/$path/$tool.zip.sha256" "$work/$tool.zip.sha256"
            sum="$(awk '{print $1}' "$work/$tool.zip.sha256")"
            verify "$work/$tool.zip" "$sum"
            (cd "$work" && unzip -q -o "$tool.zip")
            cp "$work/$tool" "$dest/$tool"
            chmod +x "$dest/$tool"
            sums="$sums
               $tool.zip  $sum"
        done
        # These builds come without a licence file; the text is simply the GPLv3.
        fetch "https://www.gnu.org/licenses/gpl-3.0.txt" "$dest/LICENSE.txt"

        provenance="Built by:      ffmpeg.martin-riedl.de, FFmpeg 9.0.1 release
Archives:      $RIEDL/$path/ffmpeg.zip
               $RIEDL/$path/ffprobe.zip
SHA-256:$sums
Build scripts: https://git.martin-riedl.de/ffmpeg/build-script"
        ;;

    *)
        echo "unknown platform: $platform" >&2
        exit 2
        ;;
esac

# The exact version and configure line, when the binary can run on this machine.
ffbin="$dest/ffmpeg"
[ -f "$ffbin.exe" ] && ffbin="$ffbin.exe"
version_info="(not captured: fetched on a different platform than it targets)"
if out="$("$ffbin" -hide_banner -version 2>/dev/null)"; then
    version_info="$out"
fi

cat > "$dest/FFMPEG-SOURCE-OFFER.txt" <<EOF
FFmpeg -- licence and source offer
==================================

Video Converter bundles the FFmpeg command-line tools ffmpeg and ffprobe
(and, on Windows, the FFmpeg shared libraries they use). The app runs them as
separate programs and talks to them only through command-line arguments and
pipes; it does not link against them.

These binaries are built with --enable-gpl --enable-version3 and include
GPL-licensed libraries such as libx264 and libx265, so they are distributed
under the GNU General Public License, version 3 or later. The full licence
text is in LICENSE.txt in this folder. No modifications were made to FFmpeg.

Where these binaries came from
------------------------------
$provenance

Exact version and build configuration
-------------------------------------
$version_info

Corresponding source code
-------------------------
FFmpeg:           https://ffmpeg.org/download.html
                  https://git.ffmpeg.org/ffmpeg.git
Release tarballs: https://ffmpeg.org/releases/

The build scripts linked above pin the exact versions of FFmpeg and of every
library compiled into it, and fetch their complete sources.

If you received this program without access to the above, you may request the
complete corresponding source code for these binaries, for at least three
years from the date of the release you downloaded, by opening an issue at
$REPO_URL/issues
EOF

echo "ffmpeg for $platform is ready in $dest"
ls -la "$dest"
