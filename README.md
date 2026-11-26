# Tiny Browser

A from-scratch web browser for Windows in C++20. No Chromium, WebKit, Gecko, Electron, or third-party
JavaScript engine. The only dependencies are Windows system APIs:

| Concern | Implementation |
|---|---|
| HTML | Own tokenizer + tree builder (`src/html.cpp`) |
| CSS | Own parser, selectors, specificity, cascade, inheritance, media queries, custom properties (`src/css.cpp`) |
| Layout | Own block / inline / table / flexbox layout, inline-block, absolute / relative / fixed positioning, overflow clipping (`src/layout_engine.cpp`) |
| Rendering | Display list painted on the GPU with Direct2D 1.1 on a Direct3D 11 flip-model swap chain (`src/gpu.cpp`, `src/render.cpp`) |
| Text | DirectWrite (shaping, font fallback, color emoji) with word-width caches (`src/text.cpp`) |
| Networking | WinHTTP transport (HTTPS via SChannel, HTTP/2, gzip/deflate/brotli); our own redirect handling, GET/POST, Referer/Origin policy; plus `file:` and `data:` (`src/http.cpp`) |
| Cookies | Own cookie jar: third-party cookies blocked, SameSite enforced, persistent cookies DPAPI-encrypted on disk (`src/http.cpp`) |
| Forms | Text fields, passwords, checkboxes, radios, dropdowns, textareas, buttons; GET and POST submission; `<meta refresh>` (`src/shell.cpp`) |
| Images | Windows Imaging Component: PNG, JPEG, GIF, BMP, ICO, WebP (`src/http.cpp`) |
| Passwords | Save prompt after sign-in, autofill on return; DPAPI-encrypted vault that is only decrypted while in use (`src/storage.cpp`, `src/shell.cpp`) |
| JavaScript | Own engine in `src/js/`: parser, bytecode compiler, register VM, mark-and-sweep garbage collector, standard library, and a backtracking regular expression engine. Not connected to pages yet (milestone 4) |
| Shell | Tabs, address bar, history, bookmarks bar, devtools with Elements, Network, Performance and a JavaScript Console (`src/shell.cpp`) |

Release build is ~1.5 MB with no DLLs beyond what ships with Windows.

## Build

Requires Visual Studio 2022 (Desktop C++ workload) and CMake.

```powershell
.\build.ps1
.\build\Release\tinybrowser.exe https://news.ycombinator.com/
```

## Command line

```text
tinybrowser.exe [url]
tinybrowser.exe --selftest [results.txt]          # engine unit tests, exit code 0 = pass
tinybrowser.exe --js <script.js> [output.txt]     # run a script in the JavaScript engine (console, timers)
tinybrowser.exe --screenshot <url> <out.png> [--size 1280x860] [--devtools elements|network|perf|console] [--scroll N]
                [--do "click:x,y;type:text;key:enter;key:tab;key:save"] [--vault test-vault.dat]
```

`--screenshot` renders headlessly (same drawing code, offscreen target) and writes `<out.png>.txt` with timings and the
position of every form control. `--do` scripts clicks and typing so forms can be tested without a window; cookies stay in
memory in this mode. `python tests/server.py` runs a local test site on http://localhost:8765 with GET/POST forms, cookies,
a redirect and layout samples.

`python tests/run_js_tests.py` runs the JavaScript suites in `tests/js/` (language, built-ins, async, garbage collection,
regular expressions). In the browser, F12 then Console gives a prompt for the same engine; a script that runs longer than
five seconds there is stopped.

## Keyboard

Ctrl+L address bar · Ctrl+T / Ctrl+W tabs · Ctrl+Tab cycle · Ctrl+D bookmark · F12 devtools · Alt+Left/Right history · F5 reload.
Internal pages: `about:home`, `about:bookmarks`, `about:passwords`.

Data lives in `%APPDATA%\TinyBrowser\` (`bookmarks.tsv`, `passwords.dat`, `cookies.dat`).

## Roadmap

1. **Milestone 1 (done):** static HTML/CSS rendering on the GPU, tabs, history, bookmarks, DPAPI vault, devtools.
2. **Milestone 2 (mostly done):** flexbox, inline-block, positioning, overflow clipping, rounded corners, interactive forms with
   GET/POST, own cookie jar, password save + autofill, meta refresh. Still open: floats, grid, background images, inline SVG,
   `::before`/`::after`, `:hover`, find-in-page, zoom, downloads, text selection.
3. **Milestone 3 - JavaScript engine (engine built):** lexer, parser, bytecode compiler, register VM, garbage collector, rope
   strings; classes, generators, async/await, Promises, destructuring, the core standard library, own RegExp engine, devtools
   Console. Still open: ES modules, Proxy, typed arrays, BigInt, Intl, async generators, a test262 run.
4. **Milestone 4 - DOM + Web APIs (next; required by Google results, YouTube and Reddit):** DOM bindings and events, script
   loading, timers + event loop, `fetch`/XHR, storage, incremental restyle/relayout. With it: per-site controls (JavaScript,
   cookies, images), the security baseline (same-origin rules, script time and memory limits, certificate and mixed-content
   warnings, per-site exceptions), content blocker (EasyList-format lists), tab sleeping.
5. **Milestone 5 - performance, security, graphics:** GPU layer compositor, off-thread layout, HTTP cache, process isolation,
   CSP and sandboxed frames, media playback, WebGL (own GLSL-to-HLSL translator), 3D page view (page layers as a movable
   parallax), save as PDF.

Small features slotted between the engine steps: resource monitor (CPU, RAM, GPU, VRAM, GPU temperature, plus a network
monitor), private windows, reader mode, page screenshots, RSS reader, translate selection.
