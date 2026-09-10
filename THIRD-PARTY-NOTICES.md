# Third-party notices

Video Converter's own source code is licensed as described in [LICENSE](LICENSE).
The released packages also contain, or are built from, the following third-party
components. Each remains under its own license; the Commons Clause in this
project's LICENSE does **not** apply to any of them.

## Bundled in every release package

### FFmpeg — GPL v3 or later

`ffmpeg` and `ffprobe` (plus shared libraries on Windows) are shipped in the
`ffmpeg/` folder and run as **separate programs**. The app communicates with them
only through command-line arguments and pipes; it does not link against them.

The builds used are configured with `--enable-gpl --enable-version3` and include
libx264 and libx265, so they are licensed under the GNU General Public License,
version 3 or later. The full text is `ffmpeg/LICENSE.txt` in every package, and
`ffmpeg/FFMPEG-SOURCE-OFFER.txt` records the exact build and where its complete
corresponding source code can be obtained.

- Project: https://ffmpeg.org
- Source: https://git.ffmpeg.org/ffmpeg.git

### Chromium Embedded Framework (CEF) — BSD 3-Clause

Provides the application window and renders the user interface. CEF is built on
Chromium, which incorporates many further open-source components.

- CEF license: `CEF-LICENSE.txt` in every package
- Chromium and dependency credits: `CEF-CREDITS.html` in every package
- Project: https://bitbucket.org/chromiumembedded/cef

## Compiled into the application

| Component | License | Project |
|---|---|---|
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| spdlog | MIT | https://github.com/gabime/spdlog |
| {fmt} | MIT | https://github.com/fmtlib/fmt |
| Vue.js | MIT | https://github.com/vuejs/core |

Each of the MIT-licensed components carries this notice:

```
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Copyright holders: Niels Lohmann (nlohmann/json), Gabi Melman (spdlog),
Victor Zverovich and {fmt} contributors ({fmt}), Evan You and Vue contributors
(Vue.js).
