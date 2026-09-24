# Design Document: RawTherapee ImGui + WebAssembly Port

**Version:** 0.1  
**Date:** 2026-09-22  
**Status:** Draft / Proposal  
**Authors:** (to be filled)

---

## 1. Goal

Create a fully functional, browser-native version of RawTherapee that provides a desktop-like experience entirely client-side using WebAssembly.

**Primary objectives:**
- Run the complete (or near-complete) RawTherapee processing engine in the browser.
- Deliver a responsive, modern UI that feels like a native desktop application.
- Support loading, editing, and exporting raw files without a server.
- Maintain a single codebase that can also target native desktop platforms.

**Non-goals (for the initial versions):**
- Pixel-perfect recreation of the existing GTK UI.
- Full feature parity with every obscure tool and preference on day one.
- Multi-user collaboration or cloud storage integration.
- Support for very old browsers.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Browser                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  HTML/JS     │  │  Canvas /    │  │  File System     │   │
│  │  Shell       │  │  WebGL2 /    │  │  Access API /    │   │
│  │  (loader,    │  │  WebGPU      │  │  Drag & Drop     │   │
│  │   FS bridge) │  │              │  │                  │   │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘   │
│         │                 │                   │             │
│         └─────────────────┼───────────────────┘             │
│                           │                                 │
│                  ┌────────▼────────┐                        │
│                  │  WASM Module    │                        │
│                  │  (Emscripten)   │                        │
│                  │                 │                        │
│                  │  ┌───────────┐  │                        │
│                  │  │ Dear ImGui│  │                        │
│                  │  │ + Backends│  │                        │
│                  │  └─────┬─────┘  │                        │
│                  │        │        │                        │
│                  │  ┌─────▼─────┐  │                        │
│                  │  │  New UI   │  │                        │
│                  │  │  Layer    │  │                        │
│                  │  │ (rtgui-   │  │                        │
│                  │  │  imgui)   │  │                        │
│                  │  └─────┬─────┘  │                        │
│                  │        │        │                        │
│                  │  ┌─────▼─────┐  │                        │
│                  │  │ rtengine  │  │                        │
│                  │  │ + deps    │  │                        │
│                  │  └───────────┘  │                        │
│                  └─────────────────┘                        │
└─────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| UI Toolkit | Dear ImGui | Excellent Emscripten support, minimal dependencies, immediate-mode fits live preview workflow |
| Rendering | WebGL2 (primary), WebGPU (optional/future) | Broad browser support + high performance |
| Platform backend | GLFW or SDL2 via Emscripten | Official ImGui examples already exist |
| Processing core | Keep existing `rtengine` | Avoid rewriting decades of color science and demosaicing work |
| File I/O | Browser File API + Emscripten virtual FS + optional OPFS | Standard web patterns |
| Threading | Web Workers + SharedArrayBuffer (where available) | Keep UI responsive during heavy processing |
| Build system | CMake + Emscripten (`emcmake` / `emmake`) | Matches existing RawTherapee build |

---

## 3. Component Breakdown

### 3.1 Processing Core (`rtengine`)

- Retain the existing C++ codebase with minimal changes.
- Compile to WASM using Emscripten.
- Key subsystems to prioritize:
  - Raw decoding (LibRaw path + fallback)
  - Demosaicing
  - Color management (lcms2)
  - Core tools (Exposure, Shadows/Highlights, Tone Curve, Sharpening, Noise Reduction, etc.)
  - Pipeline / processing parameters (`ProcParams`)
  - Thumbnail generation
- Changes required:
  - Replace or stub any remaining GTK/GLib dependencies inside the engine.
  - Ensure all memory allocation is compatible with Emscripten’s heap.
  - Expose a clean C or C++ API surface for the UI layer (avoid deep GTK types).

### 3.2 New UI Layer (`rtgui-imgui`)

Completely new code that replaces `rtgui`.

**Major panels / windows to implement:**

| Panel | Priority | Notes |
|-------|----------|-------|
| Main window + docking | P0 | ImGui docking branch recommended |
| File browser / Filmstrip | P0 | Thumbnails + metadata |
| Preview area | P0 | Texture upload of processed image |
| Toolbox (right panel) | P0 | Collapsing headers matching current tool groups |
| Histogram | P1 | Can use ImPlot |
| History / Snapshots | P1 | |
| Batch Queue | P2 | |
| Preferences | P2 | |
| Navigator / Before-After | P1 | |
| Curve editors | P1 | Custom widgets or ImPlot |

**UI Style goals:**
- Dark theme by default (photographic work).
- Compact but readable controls.
- Keyboard-driven workflow support.
- Progressive refinement of the preview (fast low-res → full quality).

### 3.3 Platform & Browser Integration

- **Input:** Mouse, keyboard, wheel, touch (basic).
- **File loading:**
  - Drag-and-drop onto the canvas.
  - `<input type="file">` multiple selection.
  - Optional: File System Access API / Origin Private File System for persistent projects.
- **Export:** Generate blob → download or File System Access API save.
- **Progress feedback:** ImGui progress bars + status text; heavy work offloaded to workers.
- **Memory:** Explicit control of Emscripten heap size; progressive loading of large RAWs.

### 3.4 Supporting Libraries (WASM builds)

Must be compiled for `wasm32-unknown-emscripten` (or compatible):

- lcms2
- libjpeg-turbo / libjpeg
- libtiff
- libpng
- zlib
- FFTW (single precision, possibly without OpenMP first)
- Exiv2 or minimal metadata reader
- LensFun (optional for first versions)
- fmt (already used by RawTherapee)

OpenMP should be disabled or replaced with a simpler parallel model initially (std::thread + Web Workers).

---

## 4. Data & Control Flow

```
User changes slider
        │
        ▼
ImGui widget → updates ProcParams (in WASM memory)
        │
        ▼
UI requests preview update (debounced)
        │
        ▼
rtengine::process()  (or incremental tool)
        │
        ▼
Output image buffer (float or 16-bit)
        │
        ▼
Upload as WebGL/WebGPU texture
        │
        ▼
ImGui::Image() or custom draw list in preview pane
```

**Caching strategy:**
- Keep last N full-resolution results for undo/history.
- Maintain a lower-resolution preview pipeline for interactive adjustments.
- Invalidate caches intelligently when only certain tools change.

---

## 5. Implementation Phases

### Phase 0 – Foundation (2–4 weeks)
- Set up Emscripten + CMake project that builds a minimal ImGui application.
- Successfully compile a stripped-down `rtengine` (or start with LibRaw-Wasm) into the same module.
- Display a static image and a few sliders that do nothing.

### Phase 1 – Minimal Viable Editor (6–10 weeks)
- Load a RAW file via drag-drop or file picker.
- Decode → basic demosaic → display in preview.
- Implement Exposure, Black/White points, and a simple Tone Curve.
- Basic export to JPEG/PNG.
- Single-image workflow only.

### Phase 2 – Core Desktop Experience (8–12 weeks)
- Filmstrip / multi-image support.
- Full set of most-used tools (Detail, Color, Lens corrections, etc.).
- History stack and snapshots.
- Histogram + basic navigator.
- Keyboard shortcuts.
- Progressive preview (fast → high quality).

### Phase 3 – Polish & Advanced Features
- Batch queue.
- Custom processing profiles.
- Before/After and dual-view.
- Advanced curve widgets, masks (if feasible).
- WebGPU backend option.
- Persistent storage (OPFS) for projects.
- Performance tuning and memory limits handling.


---

## 6. Technical Challenges & Mitigations

| Challenge | Mitigation |
|-----------|------------|
| Large RAW memory usage | Progressive loading, tiled processing, lower-res interactive pipeline, explicit heap growth limits |
| UI freezes during processing | Offload heavy `rtengine` calls to Web Workers + SharedArrayBuffer (with fallback to asyncify) |
| Missing native file dialogs | Use browser File API + drag-drop; later File System Access API |
| Complex GTK layouts | Accept that ImGui will look different; focus on usability and density |
| OpenMP / threading model | Start single-threaded or use simple thread pool; add workers later |
| Color-managed display | Implement a basic display profile path; full soft-proofing later |
| Font / text rendering | ImGui’s built-in + optional FreeType; load a high-quality font for the web build |

---

## 7. Build & Development Workflow

```bash
# Typical developer loop
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release \
  -DWITH_GTK=OFF -DUI_BACKEND=IMGUI ...
cmake --build build-wasm -j
# Serve the generated .html/.js/.wasm
python -m http.server -d build-wasm/bin
```

- Continuous integration should produce both a native test build and a WASM artifact.
- Use Emscripten’s `-sALLOW_MEMORY_GROWTH=1` and reasonable initial memory.
- Prefer `-O2` / `-O3` + LTO for release WASM builds.

---

## 8. Success Metrics

**MVP (Phase 1) is successful when:**
- A user can open a common RAW (CR2/NEF/ARW/DNG), adjust exposure and tone curve, and download a JPEG, all inside a modern browser tab, with no server involved.

**Desktop-like experience is successful when:**
- The application supports multi-image browsing, the majority of everyday tools, history, and feels responsive on a mid-range laptop.

---

## 9. Open Questions

1. Exact subset of tools for Phase 1 vs Phase 2?
2. Do we keep the existing `ProcParams` serialization format for profile compatibility?
3. WebGPU as primary or optional backend?
4. Licensing implications of any new third-party ImGui add-ons?
5. How aggressively do we pursue a shared native + web UI long-term?

---

## 10. References

- Dear ImGui: https://github.com/ocornut/imgui
- ImGui Emscripten examples (WebGL & WebGPU)
- RawTherapee source: https://github.com/RawTherapee/RawTherapee
- LibRaw-Wasm (useful reference for raw decoding): https://github.com/ybouane/LibRaw-Wasm
- Emscripten documentation

---

## 11. Status Log

### Phase 0 – Foundation (2026-09-22)

First milestone build. Everything user-level, no UAC/admin required:

- **Toolchain**
  - CMake + Ninja via `pip install --user cmake ninja` (Python 3.14).
  - Emscripten SDK cloned to `~/emsdk` (release `6.0.10`), activated without installation.
- **Minimal ImGui app** (`app/main.cpp`, root `CMakeLists.txt`)
  - Dear ImGui (docking branch, latest) + GLFW + OpenGL3/WebGL2 backend.
  - Fullscreen ImGui dockspace host window, dark theme.
  - "Preview" pane renders a static test-pattern texture; "Toolbox" pane has idle
    Exposure / Black / White / Contrast / Saturation sliders (no-op, per spec).
  - `wasmraw.html` (19 KB) + `wasmraw.js` (150 KB) + `wasmraw.wasm` (1.1 MB), served from `build-wasm/`.
- **Raw decoding core into the same module**
  - Vendored `rtengine/libraw` compiled as a CMake target `rtlibraw` (84 translation units)
    for `wasm32-unknown-emscripten` with `LIBRAW_NODLL`, no OpenMP.
    DNG SDK / RawSpeed / X3F sources compile as no-ops (`#ifdef`-guarded).
  - Linked into the WASM app; `_libraw_init`, `_libraw_version`, `_libraw_versionNumber`
    exported from the module to survive LTO dead-stripping.
- **Build** (PowerShell): `powershell -File build.ps1`
- **Serve**: `python -m http.server -d build-wasm`

### Phase 1 milestone - "Minimal viable editor" first slice (2026-09-22)

Items 1-8 of Phase 1 done (load RAW, decode->demosaic->preview, live tone sliders,
basic export to JPEG/PNG). **In-browser acceptance passed: a real Fujifilm RAF
file was drag-dropped and rendered**, alongside the synthetic DNG tests:

- **File loading** (`app/shell.html`)
  - Custom Emscripten shell: fullscreen canvas + dark overlay hint.
  - Drag-and-drop onto the window, double-click, and Ctrl+O file picker.
  - JS reads the `File` into an `ArrayBuffer`, `_malloc`s a copy into WASM heap,
    calls `_wasm_load_raw(ptr, len)`; hint text paints before the sync decode.
- **Decode** (`app/main.cpp`, LibRaw C API already in the module)
  - `libraw_open_buffer` -> `libraw_unpack` -> `libraw_dcraw_process` ->
    `libraw_dcraw_make_mem_image`, configured for linear 16-bit sRGB output
    (`output_bps=16`, gamma 1,1, `no_auto_bright`, camera WB, AHD demosaic).
  - Box-downsampled to a float RGB preview buffer (max side 1600).
  - Make/model reported via `libraw_get_iparams`.
- **Live tone mapping** (per-frame, CPU point ops on the preview)
  - Exposure (`2^EV`), Black/White points, Contrast (midpoint multiplier),
    Saturation (luma mix), then proper sRGB encode -> RGBA8 texture.
  - `RebuildPreview()` only runs when a slider changed.
- **Export to JPEG / PNG** (`app/main.cpp`, vendored `stb_image_write.h`)
  - Tone-mapped RGBA8 preview buffer is now a module-global (`g_rgba`); the tone
    mapping is split into a pure `ToneMapPreview()` (GL-free) so export works
    off the render path (and headlessly).
  - "Export" section in the toolbox: JPEG quality slider + Export PNG / Export JPEG
    buttons (disabled until a RAW is loaded). `stbi_write_*_to_func` streams bytes
    into a wasm heap buffer; an `EM_JS` hook creates a Blob and triggers a
    `rawtherapee-export.png/.jpg` download. Status line reports size + dimensions.
  - Smoke test asserts PNG magic `\x89PNG...` and JPEG SOI `\xFF\xD8`, both pass
    (64x48: PNG 7.4 KB, JPEG 1.8 KB @ q90). Export is at preview resolution for now
    (max 1600 px); full-res export is the natural next step.
- **Build**: `powershell -File build.ps1 -Smoke` also runs a headless Node test.
- **Headless smoke test** (`smoke/`)
  - `make_test_dng.py` synthesizes a minimal uncompressed 16-bit RGGB DNG
    (gradient test pattern) without any third-party libs.
  - `smoke_main.cpp` + the module's `wasm_load_raw` compiled to a Node binary
    (`--preload-file`), asserting a successful decode + demosaic and sane
    preview statistics (verified: 64x48, 16-bit 3ch, min 0 / max 1 / mean 0.501).
- Artifacts: `wasmraw.html` (4 KB) + `wasmraw.js` (154 KB) + `wasmraw.wasm` (1.26 MB).

### Web-worker offload + expanded toolset (2026-09-22)

Decoding and full-resolution export now run on a **secondary `decoder` wasm module
inside a classic Web Worker** so the app is deployable to pure static hosting
(GitHub Pages) with no COOP/COEP headers, no SharedArrayBuffer, and no pthreads.

- **Two modules, two target executables**
  - `wasmraw` - UI module: ImGui, tone tools, zoom/pan, histogram, layout persistence.
  - `decoder` (`-sMODULARIZE -sENVIRONMENT=worker`) - LibRaw decode + export
    encoding; owns the native-resolution linear copy, returns a ~1600px preview.
  - `decoder-worker.js` is static glue (`importScripts('decoder.js')`); `locateFile`
    resolves via `new URL('.', self.location.href).href`, so project pages at
    `/repo/` work without configuration. Built and copied by `build.ps1`.
- **Message protocol** (main -> worker: `decode`, `export`; worker -> main:
  `progress`, `decoded`, `exported`, `export-error`, `error`). Preview floats are
  transferred once per decode; only encoded export bytes cross back. Live tone
  sliders remain on the main thread; export params are snapshotted at click time,
  passed as individual doubles (no shared heap).
- **Shared tone math** (`app/tone_common.h`): `tone::Params` drives both the live
  preview and the worker export, so results cannot drift.
- **Tools added**: white balance (presets / gains / click-to-pick eyedropper),
  draggable 5-point tone curve, shadows/highlights recovery, luma-based clarity
  (unsharp + radius), vignetting correction, histogram window, preview zoom/pan.
- **Emscripten notes learned in-progress**: MODULARIZE modules only expose
  `EMSCRIPTEN_KEEPALIVE` symbols plus `EXPORTED_FUNCTIONS` - `_malloc`/`_free`
  must be exported explicitly (`SHELL:-sEXPORTED_FUNCTIONS=['_malloc','_free']`);
  heap views like `HEAPU8` must be added to `EXPORTED_RUNTIME_METHODS` to appear
  on the Module instance.
- **Smoke test rewritten** against the decoder API; asserts preview statistics,
  PNG magic, JPEG magic.
- Artifacts (fresh build): `wasmraw.html/js/wasm` + `decoder.js` (62 KB) +
  `decoder.wasm` (758 KB) + `decoder-worker.js`.

### Phase 2 editor-first slice (2026-09-24)

The first Phase 2 implementation deliberately extends the current LibRaw and
shared CPU pipeline instead of importing the full GTK-dependent `rtengine`.

- **Per-image documents**: each catalog entry owns its edit state, history,
  snapshots, thumbnail, decoded preview, and metadata. The browser catalog
  retains `File` objects and queues one worker decode at a time.
- **Versioned routing**: decode, preview, and export messages carry image and
  request identity plus document revisions; stale responses are ignored.
- **Editor UI**: filmstrip, history/snapshots, navigator overview, pixel readout,
  undo/redo, zoom/navigation shortcuts, and keyboard image selection.
- **Expanded tools**: temperature/tint, vibrance, sharpening, noise reduction,
  chromatic aberration, and distortion correction share `app/tone_common.h` with
  native-resolution export.
- **Progressive rendering**: the worker sends a draft preview followed by a final
  render; local-contrast and denoise work is disabled for drafts.
- **Export selection**: native-resolution and preview-resolution PNG/JPEG export
  are both supported.
- **Validation**: `build.ps1 -Smoke` checks decode statistics, worker-side preview
  rendering, and PNG/JPEG output signatures.

Remaining Phase 2 follow-up: crop/rotate, stronger browser automation, full
profile serialization, and optional separation of preview/export workers. Full
`rtengine` integration remains a later compatibility track.

---

*This document is a living proposal. Feedback and contributions are welcome.*
