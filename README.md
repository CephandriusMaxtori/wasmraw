# RawTherapee ImGui WASM

A browser-native raw photo editor port of [RawTherapee](https://www.rawtherapee.com/),
built with Dear ImGui, LibRaw, and WebAssembly (Emscripten). Entirely client-side:
no server, no uploads, no account. Drop a RAW file in the browser and edit it.

## What works

- **Decode**: real RAW files via LibRaw (unpack -> demosaic -> linear 16-bit),
  running in a **Web Worker** so the UI never blocks. Options, ISO, shutter,
  aperture, focal length, make/model reported.
- **Live preview**: processed in CPU on a downscaled linear buffer; sliders update
  in real time.
- **Tools** (preview + full-res export share the exact same math):
  - Exposure, Black/White points, Contrast, Saturation
  - White balance: presets, per-channel gains, click-to-pick from the image
  - Tone curve (draggable 5-point editor)
  - Shadows / Highlights recovery
  - Clarity (unsharp, luma-based) with radius
  - Vignetting correction (+ lighten / - darken)
  - Histogram window (luma + RGB)
  - Preview pan / zoom (scroll, drag, double-click to fit)
- **Export**: PNG or JPEG at **full native resolution**, encoded in the worker;
  tone parameters are snapshotted from the current preview state.
- **Persistent layout**: docked window arrangement is saved to `localStorage`.
- **Two WebAssembly modules**: `wasmraw` (UI) + `decoder` (worker decoding/export) -
  requires no special HTTP headers, so it runs on plain static hosting.

## Build

Prerequisites:
- Emscripten SDK (`emsdk`), activated.
- CMake + Ninja. On Windows with a user-level Python 3.14 install these live in
  `%APPDATA%\Python\Python314\Scripts`; `build.ps1` prepends that to `PATH`.
- Node.js (only for the headless smoke test; the SDK's bundled node is used).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Smoke
```

This builds both modules into `build-wasm/` and runs the headless smoke test
(synthetic DNG decode + PNG/JPEG export assertions).

## Run locally

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
python -m http.server -d build-wasm
# open http://localhost:8000/wasmraw.html
```

(`server.py` is an equivalent static server on port 8000.)

## Deploy to GitHub Pages

The build output is fully static and uses **relative URLs only**, so it works on
both `user.github.io` and `user.github.io/repo/` project pages. Pick one:

**Option A - `docs/` folder (simplest):**

```powershell
New-Item docs -ItemType Directory -Force
Copy-Item build-wasm\* docs\
git add docs
```

Then in repo Settings -> Pages: Source = "Deploy from a branch", Branch = your
default branch, Folder = `/docs`. Commit and push.

**Option B - `gh-pages` branch:**

```powershell
New-Item gh-pages-out -ItemType Directory
Copy-Item build-wasm\* gh-pages-out\
git worktree add gh-pages gh-pages-out   # or push the folder to a gh-pages branch
```

The worker loads `decoder.js` + `decoder.wasm` relative to its own script URL, so
it keeps working under a `/repo/` path; no base-tag or server config required.

## Layout

```
app/
  shell.html        Browser shell: worker wiring, splash/progress, drag-drop
  main.cpp          ImGui app (UI module wasmraw): tone tools, zoom, histogram
  decoder.cpp       Worker module: LibRaw decode + full-res export
  decoder-worker.js Worker glue + message protocol (importScripts decoder.js)
  tone_common.h     Shared tone math - single source for preview AND export
  third_party/      stb_image_write (PNG/JPEG encoder, worker-side)
RawTherapee/        Vendored upstream source (rtengine/libraw used only)
build.ps1           Build + smoke driver
CMakeLists.txt      wasmraw + decoder targets
smoke/              Headless smoke test (synthetic DNG etc.)
```

## Notes / limitations

- White balance is post-decode linear channel gain (standard simplified WB), not a
  camera-space re-demosaic.
- Full-res export with Clarity is slower (24 MP gaussian) but stays off-thread.
- No glib/GTK or full `rtengine` pipeline yet (that's the Phase 2 horizon, see
  `RawTherapee-ImGui-WASM-Design.md`).