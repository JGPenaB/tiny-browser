# Tiny Browser

A from-scratch web browser for Windows in C++20. No Chromium, WebKit, Gecko, Electron, or third-party
JavaScript engine. The only dependencies are Windows system APIs:

| Concern | Implementation |
|---|---|
| HTML | Own tokenizer + tree builder (`src/html.cpp`) |
| CSS | Own parser, selectors, specificity, cascade, inheritance, media queries, custom properties (`src/css.cpp`) |
| Layout | Own block / inline / table / flexbox layout, inline-block, absolute / relative / fixed positioning, overflow clipping, boxes that scroll, CSS transforms (`src/layout_engine.cpp`) |
| SVG | Inline SVG: paths, basic shapes, groups, `use` and symbols, transforms, fill and stroke, viewBox fitting, drawn as Direct2D geometry (`src/svg.cpp`) |
| Rendering | Display list painted on the GPU with Direct2D 1.1 on a Direct3D 11 flip-model swap chain (`src/gpu.cpp`, `src/render.cpp`) |
| Text | DirectWrite (shaping, font fallback, color emoji) with word-width caches (`src/text.cpp`) |
| Networking | WinHTTP transport (HTTPS via SChannel, HTTP/2, gzip/deflate/brotli); our own redirect handling, GET/POST, Referer/Origin policy; plus `file:` and `data:` (`src/http.cpp`) |
| Cookies | Own cookie jar: third-party cookies blocked, SameSite enforced, persistent cookies DPAPI-encrypted on disk (`src/http.cpp`) |
| Forms | Text fields, passwords, checkboxes, radios, dropdowns, textareas, buttons; GET and POST submission; `<meta refresh>` (`src/shell.cpp`) |
| Images | Windows Imaging Component: PNG, JPEG, GIF, BMP, ICO, WebP (`src/http.cpp`) |
| Passwords | Save prompt after sign-in, autofill on return; DPAPI-encrypted vault that is only decrypted while in use (`src/storage.cpp`, `src/shell.cpp`) |
| JavaScript | Own engine in `src/js/`: parser, bytecode compiler, register VM, mark-and-sweep garbage collector, standard library, and a backtracking regular expression engine. Pages reach it through the DOM bindings in `src/script.cpp` |
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
tinybrowser.exe --js <script.js> [output.txt]     # run a script in the JavaScript engine (console, timers, agents)
tinybrowser.exe --screenshot <url> <out.png> [--size 1280x860] [--devtools elements|network|perf|console] [--scroll N]
                [--do "click:x,y;move:x,y;wheel:x,y,delta;type:text;key:enter;key:tab;key:save"] [--vault test-vault.dat]
```

`--screenshot` renders headlessly (same drawing code, offscreen target) and writes `<out.png>.txt` with timings and the
position of every form control. `--do` scripts clicks, pointer moves (`move:x,y`), the mouse wheel (`delta` as the mouse reports it: -120 is one
notch down) and typing, so pages can be tested without a window; cookies stay in memory in this mode.
`python tests/server.py` runs a local test site on http://localhost:8765 with GET/POST forms, cookies, a redirect and
layout samples.

The page tests are in `tests/pages/`. Each prints `PASS` and `FAIL` lines and a `SUMMARY` into `<out.png>.txt`.
`dom.html`, `components.html`, `layout.html`, `svg.html`, `generated.html`, `floats.html`, `selectors.html`, `counters.html`, `grid.html` and `cascade.html` are opened as `file:` URLs; `net.html`, `modules.html`, `late.html` and `history.html`
come from the test server (`http://localhost:8765/pages/...`). `layout.html` also wants
`--do "wheel:300,300,-120;click:300,300;click:300,122;click:620,124"`: the wheel must log `WHEEL 100 of 100`, the
first click `CLICK wheel 6`, the second click, which lands on a row that was scrolled out of view, nothing, and the
third, on a span inside a button, `BUTTON submitted by iconButton`. `selectors.html` also wants
`--do "move:150,200;click:150,200;move:150,60"`, after which its `HOVER` lines go from grey to green with a blue
child (hovered), then a red text (pressed), then grey with the red text (pressed, no longer hovered).

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
   GET/POST, own cookie jar, password save + autofill, meta refresh, `::before` and `::after` with `content` (strings, escapes,
   `attr()`, quotes; `tests/pages/generated.html`), background images and gradients in layers (linear, radial, conic and
   repeating; position, size, repeat, origin, clip; `tests/pages/backgrounds.html` is checked by eye), floats with
   `clear` and block formatting contexts (CSS 2 9.5; `tests/pages/floats.html`), the Selectors 4 pseudo-classes: `:is()`,
   `:where()`, `:not()` and `:has()` with selector lists, the of-type ones, `:hover`, `:active`, `:focus`,
   `:focus-within`, `:focus-visible`, `:target`, `:lang()` and the form states (`tests/pages/selectors.html`); a page is
   styled again only when a state its rules look at changes; counters (CSS Lists 3: `counter-reset`, `counter-increment`,
   `counter-set`, `counter()` and `counters()` in `content`, and the `list-item` counter that list markers show, with
   `<ol start>` and `<li value>` as the HTML hints; `tests/pages/counters.html`), and grid layout (CSS Grid 1: track lists with `fr`, `minmax()`, `fit-content()`,
   `repeat()` including `auto-fill` and `auto-fit`, named lines and areas, the `grid-template` and `grid` shorthands,
   line, span and area placement, auto-placement by rows or columns, sparse or dense, implicit tracks, the track
   sizing algorithm, and box alignment of tracks and items; `tests/pages/grid.html`). `clip-path` with the basic shapes (`inset()`, `circle()`, `ellipse()`, `polygon()`) in a reference box
   (`tests/pages/clippath.html`, checked by eye), `@supports` conditions evaluated (CSS Conditional 3 and
   `selector()`), and nested style rules (CSS Nesting 1; `tests/pages/cascade.html`). Still open: subgrid, SVG text, `clip-path: url()` and masks, find-in-page, zoom, downloads, text selection.
3. **Milestone 3 - JavaScript engine (built):** lexer, parser, bytecode compiler, register VM, garbage collector, rope
   strings; classes, generators, async/await, Promises, destructuring, the core standard library, typed arrays, own RegExp
   engine, ES modules (`import`, `export`, `import()`, top-level await, cycles, import attributes with JSON modules),
   devtools Console. Behavior follows the ECMAScript specification (ECMA-262) and is checked against the test262
   conformance suite: see "Conformance" below. Temporal (every CLDR calendar, every IANA time zone) and Intl (ECMA-402,
   every service) are there too.
4. **Milestone 4 - DOM + Web APIs (in progress; required by Google results, YouTube and Reddit):** done so far: DOM
   bindings and events, classic and module scripts (import maps, `defer`, `nomodule`), timers + event loop, `fetch`/XHR with
   CORS, storage, cookies, MutationObserver, template contents, custom elements, shadow DOM (slots, scoped styles with
   `:host` and `::slotted()`, declarative shadow roots, style sheets made by script and adopted). These are checked by
   `tests/pages/components.html`, whose expectations were compared with a reference browser. Style sheets and pictures
   that a script adds after the page was loaded are fetched in the background and applied when they arrive, with `load`
   and `error` events (`tests/pages/late.html`, served by `tests/server.py`), and IntersectionObserver and
   ResizeObserver report for real. CSS follows the W3C specifications, as JavaScript follows ECMA-262:
   `getComputedStyle` returns resolved values (CSSOM), `max-height` and `min-height` bound a box as CSS 2 section 10.7
   says, column flex containers shrink their items and flex containers size themselves to their items (Flexbox 9.7
   and 9.9), `overflow: auto` boxes scroll with the wheel and `scrollTop`, fixed boxes stay in the viewport, and
   scrolling fires `scroll` events at the element or the document (`tests/pages/layout.html`). Known gaps there:
   margins, paddings and insets read as computed values (a percentage or `auto` is not turned into px), boxes scroll
   only vertically and their thumb cannot be dragged, and there is no `scrollend` event. Still open: incremental restyle/relayout, per-site controls (JavaScript, cookies, images), the security baseline
   (script time and memory limits, certificate and mixed-content warnings, per-site exceptions), content blocker
   (EasyList-format lists), tab sleeping.
5. **Milestone 5 - performance, security, graphics:** GPU layer compositor, off-thread layout, HTTP cache, process isolation,
   CSP and sandboxed frames, media playback, WebGL (own GLSL-to-HLSL translator), 3D page view (page layers as a movable
   parallax), save as PDF.

## Conformance

`python tests/run_test262.py [path filter ...] [--jobs N] [--save name]` runs the ECMAScript conformance suite (test262,
cloned into the git-ignored `third_party/test262`; the script's header has the command) and writes the failures to
`out/test262-<name>.txt`. Seventeenth full run, 2026-11-28: 48,522 of 48,803 tests pass (99.4%), Temporal's 4,616
included. Every test of ECMA-262 itself passes.

What the engine covers: the whole core language with its early errors (classes with private members and static
blocks, generators and async generators, destructuring, modules, direct and indirect `eval`, `with`, Annex B, proper
tail calls, `using` and `await using`), Proxy and Reflect, BigInt, typed arrays with resizable and shared buffers,
Atomics (with waiting and notifying between agents: threads that have an engine each and share memory), RegExp (named groups, lookbehind, Unicode properties, the `v` flag with set operations and the emoji sequence properties), Promise combinators,
iterator helpers (including `concat`, `zip`, `chunks`, `windows`), Set methods, `Array.fromAsync`,
`Error.prototype.stack`, `JSON.rawJSON`, `Math.sumPrecise`, DisposableStack, mapped `arguments` objects, import
attributes with JSON modules, and several realms in one engine (each with its own global object and built-ins, which
is what frames in a page will use). Of the proposals: Temporal, ShadowRealm, and importing bytes.

Temporal and Intl are written in JavaScript (`src/js/temporal/*.js`, `src/js/intl/*.js` and the time zone table both
use, `src/js/shared/*.js`), following their specifications' algorithms step by step. Each is compiled the first time a
script reads its global (or calls a method that needs it, such as `Date.prototype.toLocaleString`), so pages that never
use them pay nothing. After changing those files, run `python tools/gen_js_parts.py` to regenerate
`src/js/temporal_js.h` and `src/js/intl_js.h`. They take the built-ins they use from a snapshot made when the realm is
set up (`prelude.cpp snapshotIntrinsics`), so a page that replaced `Array.prototype.sort` cannot change them.

Locale data, time zone rules and the formatters themselves come from ICU, the Unicode library that Windows 10 (1903)
and later ship as `icu.dll`; `src/js/icu.cpp` is the bridge. Temporal's calendars follow the Intl era and month code
proposal: the Hebrew, tabular Islamic, Coptic, Ethiopic, Indian and ISO-month calendars are computed in JavaScript
(`src/js/temporal/3b_calendars.js`), the Chinese, Korean, Persian and Umm al-Qura ones are read from ICU.
`intl402` test262, 2026-11-28: 3,349 of 3,394. What fails needs locale or calendar data newer than the ICU 72 of this
Windows: a few Chinese new years a day off, the `gara` digits, `und-150` as `en-Latn-150`, the 2024 zone links.

What still fails are proposals only, largest first: `import defer` and source-phase imports (about
260), decorators (22).

The Unicode tables are generated: `tools/gen_unicode.py` and `tools/gen_casing.py` read the test262 clone, and
`tools/gen_emoji.py` reads Unicode's `emoji-sequences.txt` and `emoji-zwj-sequences.txt`, downloaded into the
git-ignored `third_party/unicode-emoji`.

## Engine speed

JavaScript engine speed, after the spec-compliance work and without a JIT: hidden classes (shapes) with inline caches,
lazy function parsing plus an on-disk bytecode cache, computed-goto style dispatch. Values are already NaN-boxed. Later
candidates: generational GC, superinstructions, background parsing.

WebAssembly: an engine written from scratch (an interpreter, no JIT), reached through the JavaScript `WebAssembly`
API, with a global switch and a per-site switch to turn it off, the same way JavaScript can be turned off.

Small features slotted between the engine steps: resource monitor (CPU, RAM, GPU, VRAM, GPU temperature, plus a network
monitor), private windows, reader mode, page screenshots, RSS reader, translate selection, a frameless window with the tabs
in the title strip, and a headless mode driven by JSON commands for automated scripts and AI agents.
