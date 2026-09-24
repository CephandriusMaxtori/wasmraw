# RawTherapee ImGui WASM

A browser-native raw photo editor port of [RawTherapee](https://www.rawtherapee.com/),
built with Dear ImGui, LibRaw, and WebAssembly (Emscripten). Entirely client-side:
no server, no uploads, no account. Drop a RAW file in the browser and edit it.

## What works

- **Decode**: real RAW files via LibRaw (unpack -> demosaic -> linear 16-bit),
  running in a **Web Worker** so the UI never blocks. Options, ISO, shutter,
  aperture, focal length, make/model reported.
- **Multi-image workflow**: load several files from the picker or drag/drop,
  browse them in a thumbnail filmstrip, and keep edits, history, and snapshots
  per image. Decoding is queued through the worker.
- **Live preview**: processed on a downscaled linear buffer; draft and final
  quality generations are rendered in the worker, with stale results discarded.
- **Tools** (preview + export share the same math):
  - Exposure, Black/White points, Contrast, Saturation
  - White balance: presets, per-channel gains, click-to-pick from the image
  - Tone curve (draggable 5-point editor)
  - Shadows / Highlights recovery
  - Clarity, sharpening, and noise reduction
  - Temperature, tint, vibrance
  - Chromatic aberration and distortion correction
  - Vignetting correction (+ lighten / - darken)
  - Histogram window (luma + RGB)
  - Navigator overview, pixel readout, preview pan / zoom
- **History**: per-image undo/redo and named snapshots, with keyboard shortcuts.
- **Export**: PNG or JPEG at native or preview resolution, encoded in the worker;
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
(synthetic DNG decode, worker-side preview rendering, and PNG/JPEG export assertions).

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
  shell.html        Browser shell: catalog, worker wiring, drag/drop, previews
  main.cpp          ImGui app: documents, tools, history, navigator, filmstrip
  decoder.cpp       Worker module: LibRaw decode, preview render, export
  decoder-worker.js Worker glue + versioned message protocol
  tone_common.h     Shared processing math for preview AND export
  third_party/      stb_image_write (PNG/JPEG encoder, worker-side)
RawTherapee/        Vendored upstream source (rtengine/libraw used only)
build.ps1           Build + smoke driver
CMakeLists.txt      wasmraw + decoder targets
smoke/              Headless smoke test (synthetic DNG etc.)
```

## Notes / limitations

- White balance is post-decode linear channel gain (standard simplified WB), not a
  camera-space re-demosaic.
- Detail, color, and lens tools are practical CPU approximations; the full
  `rtengine` color pipeline and profile compatibility are deferred.
- Crop/rotate, masks, batch processing, preferences, and project persistence are
  not implemented yet.
- Full-res export with local-contrast tools is slower but stays off the UI thread.
- No glib/GTK or full `rtengine` pipeline is linked into the WASM build.
