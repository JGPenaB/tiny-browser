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
3. **Milestone 3 - JavaScript engine (built):** lexer, parser, bytecode compiler, register VM, garbage collector, rope
   strings; classes, generators, async/await, Promises, destructuring, the core standard library, typed arrays, own RegExp
   engine, ES modules (`import`, `export`, `import()`, top-level await, cycles, import attributes with JSON modules),
   devtools Console. Behavior follows the ECMAScript specification (ECMA-262) and is checked against the test262
   conformance suite: see "Conformance" below. Still open: Temporal, `using` declarations, Intl.
4. **Milestone 4 - DOM + Web APIs (in progress; required by Google results, YouTube and Reddit):** done so far: DOM
   bindings and events, classic and module scripts (import maps, `defer`, `nomodule`), timers + event loop, `fetch`/XHR with
   CORS, storage, cookies, MutationObserver. Still open: custom elements and shadow DOM (Reddit), styles and images added
   by script, incremental restyle/relayout, per-site controls (JavaScript, cookies, images), the security baseline
   (script time and memory limits, certificate and mixed-content warnings, per-site exceptions), content blocker
   (EasyList-format lists), tab sleeping.
5. **Milestone 5 - performance, security, graphics:** GPU layer compositor, off-thread layout, HTTP cache, process isolation,
   CSP and sandboxed frames, media playback, WebGL (own GLSL-to-HLSL translator), 3D page view (page layers as a movable
   parallax), save as PDF.

## Conformance

`python tests/run_test262.py [path filter ...] [--jobs N] [--save name]` runs the ECMAScript conformance suite (test262,
cloned into the git-ignored `third_party/test262`; the script's header has the command) and writes the failures to
`out/test262-<name>.txt`. Eleventh full run, 2026-11-27: 43,504 of 48,803 tests pass (89.1%). Without the 4,616 tests
for Temporal, a proposal that is not part of ECMA-262 yet, that is 98.5%.

What the engine covers: the whole core language with its early errors (classes with private members and static
blocks, generators and async generators, destructuring, modules, direct and indirect `eval`, `with`, Annex B, proper
tail calls, `using` and `await using`), Proxy and Reflect, BigInt, typed arrays with resizable and shared buffers,
Atomics, RegExp (named groups, lookbehind, Unicode properties, the `v` flag with set operations), Promise combinators,
iterator helpers (including `concat`, `zip`, `chunks`, `windows`), Set methods, `Array.fromAsync`,
`Error.prototype.stack`, `JSON.rawJSON`, `Math.sumPrecise`, DisposableStack, mapped `arguments` objects, import
attributes with JSON modules.

What still fails, largest first: Temporal (4,616, a proposal), `import defer` and source-phase imports (about 250, both
proposals), tests that need a second realm (about 190), Atomics tests that need worker agents (112), ShadowRealm (64, a
proposal), decorators (20, a proposal), emoji sequence properties in regular expressions (13, they need Unicode data
files).

## Engine speed

JavaScript engine speed, after the spec-compliance work and without a JIT: hidden classes (shapes) with inline caches,
lazy function parsing plus an on-disk bytecode cache, computed-goto style dispatch. Values are already NaN-boxed. Later
candidates: generational GC, superinstructions, background parsing.

WebAssembly: an engine written from scratch (an interpreter, no JIT), reached through the JavaScript `WebAssembly`
API, with a global switch and a per-site switch to turn it off, the same way JavaScript can be turned off.

Small features slotted between the engine steps: resource monitor (CPU, RAM, GPU, VRAM, GPU temperature, plus a network
monitor), private windows, reader mode, page screenshots, RSS reader, translate selection, a frameless window with the tabs
in the title strip, and a headless mode driven by JSON commands for automated scripts and AI agents.
