param(
    [switch]$Smoke
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$scripts = Join-Path $env:APPDATA "Python\Python314\Scripts"
$env:PATH = "$scripts;$env:PATH"
$em = Join-Path $env:USERPROFILE "emsdk\upstream\emscripten"
$node = Join-Path $env:USERPROFILE "emsdk\node\24.19.0_64bit\node.exe"
$imgui = Join-Path $env:USERPROFILE "imgui"
$toolchain = Join-Path $em "cmake\Modules\Platform\Emscripten.cmake"
$lr = Join-Path $root "RawTherapee\rtengine\libraw"

if (-not (Test-Path $toolchain)) {
    Write-Host "Emscripten SDK not found. Install with:" -ForegroundColor Red
    Write-Host "  git clone https://github.com/emscripten-core/emsdk.git $env:USERPROFILE\emsdk"
    Write-Host "  & $env:USERPROFILE\emsdk\emsdk.bat install latest"
    Write-Host "  & $env:USERPROFILE\emsdk\emsdk.bat activate latest"
    exit 1
}

cmake -G Ninja -B build-wasm -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" .
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build build-wasm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Static worker bootstrap + decoder.js must sit next to wasmraw.html
Copy-Item (Join-Path $root "app\decoder-worker.js") (Join-Path $root "build-wasm\decoder-worker.js") -Force

Write-Host ""
Write-Host "Built. Serve build-wasm and open wasmraw.html:" -ForegroundColor Green
Write-Host "  python -m http.server -d build-wasm"
Write-Host "Acceptance (Phase 1): decode a RAW via drag-drop or double-click"
if (-not $Smoke) { exit 0 }

# --- Headless decode smoke test (Node) ---
Write-Host "" -ForegroundColor Green
Write-Host "Running smoke test ..." -ForegroundColor Green
Push-Location (Join-Path $root "smoke")
try {
    python make_test_dng.py
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $em "em++.exe") -O2 `
        -I"$root\app" -I"$root\app\third_party" `
        -I"$lr" -I"$lr\libraw" `
        "$root\app\decoder.cpp" "$root\smoke\smoke_main.cpp" "$root\app\third_party\stb_image_write.cpp" `
        (Join-Path $root "build-wasm\librtlibraw.a") `
        --preload-file "test.dng@/test.dng" `
        -o "smoke.js" -sEXPORTED_FUNCTIONS=_main -sNO_EXIT_RUNTIME=1
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $node "smoke.js" "test.dng"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
Write-Host "Smoke test passed" -ForegroundColor Green