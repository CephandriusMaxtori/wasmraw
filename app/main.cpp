#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "tone_common.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "emscripten_mainloop_stub.h"
#endif

// ---------- Live tone sliders ----------
static float g_exposure = 0.0f;    // EV
static int   g_black = 0;          // 0..50  -> fraction 0..0.5
static int   g_white = 100;        // 50..100 -> fraction 0.5..1.0
static float g_contrast = 0.0f;    // -100..100
static float g_saturation = 100.0f;// 0..200
static int   g_vignette = 0;       // -100..100 -> -1..1
static float g_clarity = 0.0f;     // -100..100 -> -1..1
static float g_clarityRadius = 2.0f;// 0.5..3 px
static int   g_shadows = 0;        // -100..100
static int   g_highlights = 0;     // -100..100
static int   g_jpegQuality = 90;   // 0..100

// ---------- White balance (linear channel gain applied at tone-map time) ----------
static float g_wb[3] = { 1.0f, 1.0f, 1.0f };
static const char* kWbPresets[] = { "Camera", "Daylight", "Cloudy", "Shade",
                                    "Tungsten", "Fluorescent", "Flash", "Custom" };
static const float kWbPresetValues[7][3] = {
    { 1.00f, 1.00f, 1.00f },  // Camera
    { 1.00f, 1.00f, 1.00f },  // Daylight
    { 0.98f, 0.98f, 1.12f },  // Cloudy
    { 0.92f, 1.00f, 1.20f },  // Shade
    { 0.82f, 1.00f, 1.48f },  // Tungsten
    { 1.00f, 1.05f, 1.12f },  // Fluorescent
    { 1.00f, 1.00f, 1.04f },  // Flash
};
static int   g_wbPreset = 0;
static bool  g_wbPicking = false;

// ---------- Tone curve (x fixed at 0,.25,.5,.75,1; y movable in 0..1) ----------
static float g_curveY[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

// ---------- Histogram (computed from the tone-mapped preview) ----------
static int g_histL[256], g_histR[256], g_histG[256], g_histB[256];
static bool g_histDirty = true;

// ---------- Decoded RAW (preview-resolution linear float RGB) ----------
struct DecodedImage {
    bool loaded = false;
    int w = 0, h = 0, colors = 0, bits = 0;
    int pw = 0, ph = 0;
    std::vector<float> rgb;        // pw*ph*3, linear [0..1]
    bool hasFullRes = false;       // full-res lives in the decoder worker
    int fw = 0, fh = 0;
    char make[64] = {0};
    char model[64] = {0};
    char lens[80] = {0};
    float iso = 0.f, shutter = 0.f, aperture = 0.f, focal = 0.f;
    int lastError = 0;
    char lastStatus[256] = "No image loaded - drop a RAW file or double-click";
};
static DecodedImage g_dec;

static GLuint g_tex = 0;
static int g_texW = 0, g_texH = 0;
static bool g_previewDirty = true;

// ---------- Preview pan/zoom ----------
static bool g_zoomFit = true;
static float g_zoom = 1.0f;      // image px per CSS px when not fitting
static ImVec2 g_pan(0.0f, 0.0f); // CSS px offset from the centered position

static void ResetPreviewView()
{
    g_zoomFit = true;
    g_zoom = 1.0f;
    g_pan = ImVec2(0.0f, 0.0f);
}

// Tone-mapped RGBA8 preview buffer (also the upload source for the GL texture)
static std::vector<unsigned char> g_rgba;    // pw*ph*4
static char g_exportStatus[160] = "";        // filled from the worker's result

static const int kPreviewMaxDim = 1600;
static bool g_exportFullRes = true;  // export native resolution when available

// ---------- Placeholder test pattern (shown until a RAW is loaded) ----------
static void CreatePlaceholderTexture()
{
    const int w = 640, h = 428;
    std::vector<unsigned char> pixels((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            int bar = x * 8 / w;
            unsigned char r = 0, g = 0, b = 0;
            switch (bar) {
                case 0: r = g = b = 255; break;
                case 1: r = g = 255; break;
                case 2: g = b = 255; break;
                case 3: g = 255; break;
                case 4: r = b = 255; break;
                case 5: r = 255; break;
                case 6: b = 255; break;
                default: break;
            }
            if (y > h * 3 / 4) {
                bool check = (((x / 16) + (y / 16)) % 2) == 0;
                r = g = b = check ? 220 : 40;
            }
            pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = 255;
        }
    }
    if (g_tex) glDeleteTextures(1, &g_tex);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g_texW = w; g_texH = h;
}

// ---------- Preview rebuild: linear float RGB -> RGBA8 with tone sliders ----------
static tone::Params CurrentToneParams()
{
    tone::Params p;
    p.exposure = g_exposure;
    p.black = (float)g_black / 100.0f;
    p.white = (float)g_white / 100.0f;
    p.contrast = g_contrast / 100.0f;
    p.saturation = g_saturation / 100.0f;
    p.vignette = (float)g_vignette / 100.0f;
    p.clarity = g_clarity / 100.0f;
    p.clarityRadius = g_clarityRadius;
    p.shadows = (float)g_shadows / 100.0f;
    p.highlights = (float)g_highlights / 100.0f;
    p.wbR = g_wb[0]; p.wbG = g_wb[1]; p.wbB = g_wb[2];
    for (int i = 0; i < 5; ++i) p.curveY[i] = g_curveY[i];
    return p;
}

static void ToneMapPreview()
{
    if (!g_dec.loaded || g_dec.pw <= 0 || g_dec.ph <= 0) return;
    tone::ToneMapToBuffer(g_dec.rgb, g_dec.pw, g_dec.ph, CurrentToneParams(), g_rgba);
}

static void RebuildPreview()
{
    ToneMapPreview();
    const int w = g_dec.pw, h = g_dec.ph;
    if (w <= 0 || h <= 0) return;
    const unsigned char* dst = g_rgba.data();

    if (!g_tex) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_texW = g_texH = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex);
    }
    if (w != g_texW || h != g_texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        g_texW = w; g_texH = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    }
    g_previewDirty = false;
    g_histDirty = true;
}

// ---------- WASM API for the JS shell ----------
// Decode and full-resolution export run in the decoder worker. This module
// receives the preview floats + metadata the worker produced, and forwards
// export requests / results to/from the worker.

extern "C" {

// Preview floats are pw*ph*3 float32 in module memory, plus metadata.
EMSCRIPTEN_KEEPALIVE
void wasm_accept_decode(const float* preview, int w, int h, int pw, int ph,
                        double iso, double shutter, double aperture, double focal,
                        const char* make, const char* model, const char* lens,
                        const char* status)
{
    g_dec.loaded = true;
    g_dec.w = w; g_dec.h = h;
    g_dec.pw = pw; g_dec.ph = ph;
    g_dec.colors = 3;
    g_dec.bits = 16;
    g_dec.hasFullRes = false; // full-res copy lives in the worker
    g_dec.fw = w; g_dec.fh = h;
    g_dec.iso = (float)iso;
    g_dec.shutter = (float)shutter;
    g_dec.aperture = (float)aperture;
    g_dec.focal = (float)focal;
    snprintf(g_dec.make, sizeof(g_dec.make), "%s", make ? make : "");
    snprintf(g_dec.model, sizeof(g_dec.model), "%s", model ? model : "");
    snprintf(g_dec.lens, sizeof(g_dec.lens), "%s", lens ? lens : "");
    snprintf(g_dec.lastStatus, sizeof(g_dec.lastStatus), "%s", status ? status : "Decoded");
    g_dec.rgb.assign(preview, preview + (size_t)pw * ph * 3);
    g_dec.lastError = 0;
    ResetPreviewView();
    g_previewDirty = true;
    g_histDirty = true;
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_export_status(const char* text)
{
    snprintf(g_exportStatus, sizeof(g_exportStatus), "%s", text ? text : "");
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_request_export, (int fmt, double exp, double black, double white,
                                double contrast, double sat,
                                double wbR, double wbG, double wbB, double vignette,
                                double clarity, double clarityRadius,
                                double shadows, double highlights,
                                double cy0, double cy1, double cy2, double cy3, double cy4,
                                double quality), {
    if (typeof worker === 'undefined' || !worker) return;
    worker.postMessage({
        cmd: 'export', fmt: fmt,
        exp: exp, black: black, white: white,
        contrast: contrast, sat: sat,
        wbR: wbR, wbG: wbG, wbB: wbB, vignette: vignette,
        clarity: clarity, clarityRadius: clarityRadius,
        shadows: shadows, highlights: highlights,
        cy0: cy0, cy1: cy1, cy2: cy2, cy3: cy3, cy4: cy4,
        quality: Math.floor(quality)
    });
});
#endif

static void RequestExport(int format)
{
    if (!g_dec.loaded) return;
    snprintf(g_exportStatus, sizeof(g_exportStatus), "Exporting...");
#ifdef __EMSCRIPTEN__
    js_request_export(format,
                      g_exposure,
                      (double)g_black / 100.0,
                      (double)g_white / 100.0,
                      (double)g_contrast / 100.0,
                      (double)g_saturation / 100.0,
                      g_wb[0], g_wb[1], g_wb[2],
                      (double)g_vignette / 100.0,
                      (double)g_clarity / 100.0,
                      (double)g_clarityRadius,
                      (double)g_shadows / 100.0,
                      (double)g_highlights / 100.0,
                      g_curveY[0], g_curveY[1], g_curveY[2], g_curveY[3], g_curveY[4],
                      g_jpegQuality);
#else
    (void)format;
#endif
}

} // extern "C"

// ---------- Layout persistence (dock/window layout via localStorage) ----------
#ifdef __EMSCRIPTEN__
EM_JS(void, js_set_layout, (const char* src, int len), {
    try {
        localStorage.setItem('wasmraw-imgui-ini', UTF8ToString(src, len));
    } catch (e) { /* quota / privacy mode: ignore */ }
});

EM_JS(int, js_get_layout, (char* dst, int maxlen), {
    try {
        var s = localStorage.getItem('wasmraw-imgui-ini');
        if (!s) return 0;
        if (s.length >= maxlen) s = s.substr(0, maxlen - 1);
        stringToUTF8(s, dst, maxlen);
        return s.length;
    } catch (e) { return 0; }
});

// Layout in CSS pixels, render at devicePixelRatio: {cssW, cssH, dpr}
EM_JS(void, js_get_viewport, (double* out), {
    var dpr = window.devicePixelRatio || 1;
    HEAPF64[out / 8 + 0] = window.innerWidth;
    HEAPF64[out / 8 + 1] = window.innerHeight;
    HEAPF64[out / 8 + 2] = dpr;
});

static void ApplyDisplayScale()
{
    double vp[3];
    js_get_viewport(vp);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)vp[0], (float)vp[1]);
    io.DisplayFramebufferScale = ImVec2((float)vp[2], (float)vp[2]);
}

static void SaveLayoutToStorage()
{
    size_t len = 0;
    const char* s = ImGui::SaveIniSettingsToMemory(&len);
    if (s && len > 0) js_set_layout(s, (int)len);
}

static void RestoreLayoutFromStorage()
{
    static char s_buffer[256 * 1024];
    int len = js_get_layout(s_buffer, sizeof(s_buffer));
    if (len > 0) ImGui::LoadIniSettingsFromMemory(s_buffer, (size_t)len);
}
#endif

// ---------- UI ----------
// Small draggable tone-curve editor; draws into an invisible button.
static bool CurveEditor(const char* id, float y[5])
{
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y = 150.0f;
    if (size.x < 40.0f) size.x = 40.0f;
    ImGui::InvisibleButton(id, size);
    ImVec2 o = ImGui::GetItemRectMin();
    ImVec2 sz = ImGui::GetItemRectSize();
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = io.MousePos;
    static int dragIdx = -1;
    bool changed = false;

    auto pt = [&](int i) {
        return ImVec2(o.x + sz.x * (float)i * 0.25f, o.y + sz.y * (1.0f - y[i]));
    };

    if (hovered && io.MouseClicked[0]) {
        dragIdx = -1;
        float best = 14.0f;
        for (int i = 0; i < 5; ++i) {
            ImVec2 p = pt(i);
            float d = sqrtf((m.x - p.x) * (m.x - p.x) + (m.y - p.y) * (m.y - p.y));
            if (d < best) { best = d; dragIdx = i; }
        }
    }
    if (dragIdx >= 0 && ImGui::IsMouseDown(0)) {
        float ny = 1.0f - (m.y - o.y) / sz.y;
        ny = ny < 0.0f ? 0.0f : (ny > 1.0f ? 1.0f : ny);
        if (ny < y[dragIdx]) ny = y[dragIdx] > 0.05f ? ny : y[dragIdx]; // never pin below visible
        if (dragIdx > 0 && ny < y[dragIdx - 1]) ny = y[dragIdx - 1];   // keep monotonic
        if (dragIdx < 4 && ny > y[dragIdx + 1]) ny = y[dragIdx + 1];
        if (ny != y[dragIdx]) { y[dragIdx] = ny; changed = true; }
    } else if (dragIdx >= 0) {
        dragIdx = -1;
    }
    if (hovered && io.MouseDoubleClicked[0]) {
        for (int i = 0; i < 5; ++i) y[i] = (float)i * 0.25f;
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), IM_COL32(30, 30, 36, 255));
    for (int i = 0; i <= 4; ++i) {
        float x = o.x + sz.x * (float)i * 0.25f;
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + sz.y), IM_COL32(255, 255, 255, 28));
    }
    for (int i = 0; i <= 4; ++i) {
        float yy = o.y + sz.y * (float)i * 0.25f;
        dl->AddLine(ImVec2(o.x, yy), ImVec2(o.x + sz.x, yy), IM_COL32(255, 255, 255, 16));
    }
    for (int i = 0; i < 4; ++i) {
        ImVec2 a = pt(i), b = pt(i + 1);
        dl->AddLine(a, b, IM_COL32(255, 140, 60, 255), 2.0f);
    }
    for (int i = 0; i < 5; ++i) {
        ImVec2 p = pt(i);
        dl->AddCircleFilled(p, 4.5f, IM_COL32(255, 255, 255, 255));
        dl->AddCircleFilled(p, 2.5f, IM_COL32(255, 140, 60, 255));
    }
    return changed;
}

static void AddHistPoly(ImDrawList* dl, float x0, float y0, float x1, float y1,
                        const int* h, ImU32 col)
{
    int maxv = 1;
    for (int i = 0; i < 256; ++i) if (h[i] > maxv) maxv = h[i];
    std::vector<ImVec2> pts;
    pts.reserve(256);
    for (int i = 0; i < 256; ++i) {
        float f = h[i] ? sqrtf((float)h[i] / (float)maxv) : 0.0f;
        pts.emplace_back(x0 + (x1 - x0) * (float)i / 255.0f,
                         y1 - f * (y1 - y0));
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.0f);
}

static void ComputeHistogram()
{
    if (!g_dec.loaded || g_rgba.empty()) return;
    memset(g_histL, 0, sizeof(g_histL));
    memset(g_histR, 0, sizeof(g_histR));
    memset(g_histG, 0, sizeof(g_histG));
    memset(g_histB, 0, sizeof(g_histB));
    const unsigned char* s = g_rgba.data();
    size_t n = (size_t)g_dec.pw * g_dec.ph;
    for (size_t i = 0; i < n; ++i) {
        unsigned char r = s[i * 4], gg = s[i * 4 + 1], b = s[i * 4 + 2];
        int lum = ((int)r * 77 + (int)gg * 151 + (int)b * 28) >> 8;
        ++g_histL[lum]; ++g_histR[r]; ++g_histG[gg]; ++g_histB[b];
    }
    g_histDirty = false;
}

static void DrawHistogram()
{
    if (g_histDirty) ComputeHistogram();
    ImGui::Begin("Histogram");
    if (!g_dec.loaded || g_rgba.empty()) {
        ImGui::TextDisabled("No image loaded");
        ImGui::End();
        return;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 40.0f || avail.y < 40.0f) { ImGui::End(); return; }
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x1 = o.x + avail.x, y1 = o.y + avail.y;
    dl->AddRectFilled(o, ImVec2(x1, y1), IM_COL32(26, 26, 30, 255));
    AddHistPoly(dl, o.x + 1, o.y + 1, x1 - 1, y1 - 1, g_histL, IM_COL32(255, 255, 255, 200));
    AddHistPoly(dl, o.x + 1, o.y + 1, x1 - 1, y1 - 1, g_histR, IM_COL32(255, 80, 80, 70));
    AddHistPoly(dl, o.x + 1, o.y + 1, x1 - 1, y1 - 1, g_histG, IM_COL32(90, 220, 90, 70));
    AddHistPoly(dl, o.x + 1, o.y + 1, x1 - 1, y1 - 1, g_histB, IM_COL32(90, 140, 255, 70));
    ImGui::Dummy(avail);
    ImGui::End();
}

static void DrawToolbox()
{
    ImGui::Begin("Toolbox");
    ImGui::TextUnformatted("Exposure");
    ImGui::SliderFloat("##exposure", &g_exposure, -5.0f, 5.0f, "%.2f EV");
    ImGui::TextUnformatted("Black / White points");
    ImGui::SliderInt("Black", &g_black, 0, 50);
    ImGui::SliderInt("White", &g_white, 50, 100);
    ImGui::TextUnformatted("Tone");
    ImGui::SliderFloat("Contrast", &g_contrast, -100.0f, 100.0f);
    ImGui::SliderFloat("Saturation", &g_saturation, 0.0f, 200.0f);
    ImGui::SliderInt("Vignetting", &g_vignette, -100, 100, "%d");
    ImGui::TextDisabled("+ lightens corners, - darkens");
    ImGui::SliderInt("Shadows", &g_shadows, -100, 100, "%d");
    ImGui::SliderInt("Highlights", &g_highlights, -100, 100, "%d");
    ImGui::SliderFloat("Clarity", &g_clarity, -100.0f, 100.0f, "%.0f");
    ImGui::SliderFloat("Radius", &g_clarityRadius, 0.5f, 3.0f, "%.1f px");

    ImGui::Separator();
    ImGui::TextUnformatted("White balance");
    if (ImGui::BeginCombo("##wbpreset", kWbPresets[g_wbPreset])) {
        for (int i = 0; i < 8; ++i) {
            if (ImGui::Selectable(kWbPresets[i], g_wbPreset == i)) {
                if (i < 7) {
                    g_wb[0] = kWbPresetValues[i][0];
                    g_wb[1] = kWbPresetValues[i][1];
                    g_wb[2] = kWbPresetValues[i][2];
                }
                g_wbPreset = i;
                g_previewDirty = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::DragFloat3("Gains", g_wb, 0.005f, 0.20f, 3.00f, "%.2f")) {
        g_wbPreset = 7;
        g_previewDirty = true;
    }
    ImGui::Checkbox("Pick from preview (click image)", &g_wbPicking);
    if (!g_dec.loaded) ImGui::TextDisabled("Load a RAW first");

    ImGui::Separator();
    ImGui::TextUnformatted("Tone curve");
    if (ImGui::SmallButton("Reset curve")) {
        for (int i = 0; i < 5; ++i) g_curveY[i] = 0.25f * (float)i;
        g_previewDirty = true;
    }
    if (CurveEditor("##tonecurve", g_curveY)) g_previewDirty = true;
    ImGui::Separator();
    if (ImGui::Button("Reset tone")) {
        g_exposure = 0.0f; g_black = 0; g_white = 100;
        g_contrast = 0.0f; g_saturation = 100.0f; g_vignette = 0;
        g_clarity = 0.0f; g_clarityRadius = 2.0f;
        g_shadows = 0; g_highlights = 0;
        g_wb[0] = g_wb[1] = g_wb[2] = 1.0f; g_wbPreset = 0;
        for (int i = 0; i < 5; ++i) g_curveY[i] = 0.25f * (float)i;
        g_previewDirty = true;
    }
    ImGui::TextDisabled("Live CPU point ops on decoded preview");

    bool hasImage = g_dec.loaded && g_dec.pw > 0;
    ImGui::Separator();
    ImGui::TextUnformatted("Export");
    if (!hasImage) ImGui::BeginDisabled();
    ImGui::SliderInt("JPEG quality", &g_jpegQuality, 50, 100, "%d");
    if (g_dec.hasFullRes) {
        ImGui::Checkbox("Full resolution", &g_exportFullRes);
        if (g_exportFullRes)
            ImGui::TextDisabled("Native %dx%d -> tone-mapped copy",
                                g_dec.fw, g_dec.fh);
    }
    if (ImGui::Button("Export PNG")) RequestExport(0);
    ImGui::SameLine();
    if (ImGui::Button("Export JPEG")) RequestExport(1);
    if (g_exportStatus[0]) ImGui::TextWrapped("%s", g_exportStatus);
    if (!hasImage) { ImGui::EndDisabled(); ImGui::TextDisabled("Load a RAW first"); }
    ImGui::End();
}

static void DrawPreview()
{
    ImGui::Begin("Preview");
    if (!(g_tex && g_texW > 0 && g_texH > 0)) {
        ImGui::TextDisabled("No image loaded");
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::Button("Fit")) ResetPreviewView();
    ImGui::SameLine();
    if (ImGui::Button("-")) { g_zoomFit = false; g_zoom = std::max(0.05f, g_zoom / 1.25f); }
    ImGui::SameLine();
    if (ImGui::Button("+")) { g_zoomFit = false; g_zoom = std::min(16.0f, g_zoom * 1.25f); }
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", g_texW, g_texH);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float pw = (float)g_texW, ph = (float)g_texH;

    float viewScale;
    if (g_zoomFit) {
        viewScale = 1.0f;
        if (avail.x > 0.f && pw > 0.f) viewScale = std::min(viewScale, avail.x / pw);
        if (avail.y > 0.f && ph > 0.f) viewScale = std::min(viewScale, avail.y / ph);
    } else {
        viewScale = std::max(0.05f, std::min(16.0f, g_zoom));
    }

    // Invisible interactive surface covering the whole content area.
    ImGui::InvisibleButton("preview_viewarea", avail);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImVec2 origin = ImGui::GetItemRectMin();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        g_zoomFit = false;
        float from = viewScale;
        float to = std::max(0.05f, std::min(16.0f, from * powf(1.25f, ImGui::GetIO().MouseWheel)));
        if (to != from) {
            // Zoom around the cursor: keep the image point under it fixed.
            float cx = origin.x + avail.x * 0.5f;
            float cy = origin.y + avail.y * 0.5f;
            float tl0x = cx - pw * from * 0.5f + g_pan.x;
            float tl0y = cy - ph * from * 0.5f + g_pan.y;
            float tcx = (mouse.x - tl0x) / from;
            float tcy = (mouse.y - tl0y) / from;
            float tl1x = mouse.x - tcx * to;
            float tl1y = mouse.y - tcy * to;
            g_pan.x = tl1x - cx + pw * to * 0.5f;
            g_pan.y = tl1y - cy + ph * to * 0.5f;
            g_zoom = to;
            viewScale = to;
        }
    } else if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        ResetPreviewView();
    } else if (active && !g_zoomFit) {
        g_pan.x += ImGui::GetIO().MouseDelta.x;
        g_pan.y += ImGui::GetIO().MouseDelta.y;
    }

    float iw = pw * viewScale, ih = ph * viewScale;
    float cxp = origin.x + avail.x * 0.5f;
    float cyp = origin.y + avail.y * 0.5f;
    float tlx = cxp - iw * 0.5f + (g_zoomFit ? 0.0f : g_pan.x);
    float tly = cyp - ih * 0.5f + (g_zoomFit ? 0.0f : g_pan.y);

    // White-balance eyedropper: click a neutral point to sample it.
    if (g_wbPicking && hovered && ImGui::IsMouseClicked(0) && g_dec.loaded && g_dec.pw > 0) {
        float u = (mouse.x - tlx) / iw, v = (mouse.y - tly) / ih;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            int px = (int)(u * (float)(g_dec.pw - 1));
            int py = (int)(v * (float)(g_dec.ph - 1));
            if (px < 0) px = 0; if (py < 0) py = 0;
            float sum[3] = { 0, 0, 0 };
            int ns = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = px + dx, sy = py + dy;
                    if (sx < 0 || sx >= g_dec.pw || sy < 0 || sy >= g_dec.ph) continue;
                    const float* s = &g_dec.rgb[((size_t)sy * g_dec.pw + sx) * 3];
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2];
                    ++ns;
                }
            }
            if (ns > 0) {
                for (int c = 0; c < 3; ++c) sum[c] /= (float)ns;
                float gain[3];
                float mn = 1e30f;
                for (int c = 0; c < 3; ++c) {
                    float val = sum[c] > 1e-4f ? sum[c] : 1e-4f;
                    gain[c] = 1.0f / val;
                    if (gain[c] < mn) mn = gain[c];
                }
                for (int c = 0; c < 3; ++c) gain[c] /= mn;
                for (int c = 0; c < 3; ++c) g_wb[c] = gain[c];
                g_wbPreset = 7;
                g_wbPicking = false;
                g_previewDirty = true;
            }
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(20, 20, 24, 255));
    dl->AddImage((ImTextureID)(intptr_t)g_tex, ImVec2(tlx, tly), ImVec2(tlx + iw, tly + ih));

    char zbuf[48];
    snprintf(zbuf, sizeof(zbuf), "%d%%%s", (int)(viewScale * 100.0f),
             g_zoomFit ? " (fit)" : "");
    dl->AddText(ImVec2(origin.x + 6, origin.y + avail.y - 30),
                IM_COL32(255, 255, 255, 220), zbuf);

    ImGui::End();
}

static void DrawStatus()
{
    ImGui::Begin("Status");
    ImGui::TextWrapped("%s", g_dec.lastStatus);
    if (g_dec.loaded) {
        ImGui::Separator();
        ImGui::Text("Camera: %s %s", g_dec.make, g_dec.model);
        char cam[128] = "";
        if (g_dec.iso > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "ISO %.0f  ", g_dec.iso);
        if (g_dec.shutter > 0.f) {
            if (g_dec.shutter >= 1.f)
                snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "1/%.0fs  ", g_dec.shutter);
            else
                snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "%.1fs  ", g_dec.shutter);
        }
        if (g_dec.aperture > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "f/%.1f  ", g_dec.aperture);
        if (g_dec.focal > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "%.0fmm", g_dec.focal);
        if (cam[0]) ImGui::Text("%s", cam);
        if (g_dec.lens[0]) ImGui::TextDisabled("Lens: %s", g_dec.lens);
        ImGui::Separator();
        ImGui::Text("Source: %d x %d, %d-bit, %d channels",
                    g_dec.w, g_dec.h, g_dec.bits, g_dec.colors);
        ImGui::Text("Preview: %d x %d (max %d)", g_dec.pw, g_dec.ph, kPreviewMaxDim);
        ImGui::Text("EV %+.2f  Black %d  White %d  Contrast %.0f  Sat %.0f",
                    g_exposure, g_black, g_white, g_contrast, g_saturation);
    }
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::End();
}

static void RenderFrame(GLFWwindow* window)
{
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
#ifdef __EMSCRIPTEN__
    ApplyDisplayScale(); // layout in CSS px, framebuffer at devicePixelRatio
#endif
    ImGui::NewFrame();

    if (g_previewDirty && g_dec.loaded) RebuildPreview();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("HostDockspace", nullptr, hostFlags);
    ImGui::PopStyleVar(2);
    ImGuiID dockspaceId = ImGui::GetID("MainDock");
    ImGui::DockSpace(dockspaceId);
    ImGui::End();

    DrawPreview();
    DrawToolbox();
    DrawHistogram();
    DrawStatus();

    ImGui::Render();
    int display_w = 0, display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

static void MarkToneDirty() { g_previewDirty = true; }

#if !defined(WASMRAW_SMOKE_TEST)
int main()
{
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "RawTherapee WASM", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // no filesystem in browser; layout via localStorage
#ifdef __EMSCRIPTEN__
    RestoreLayoutFromStorage();
#endif

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 300 es");
#ifdef __EMSCRIPTEN__
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif

    CreatePlaceholderTexture();

    // Track slider edits to avoid pointless per-frame rebuilds
    float lastExposure = g_exposure;
    int lastBlack = g_black, lastWhite = g_white;
    float lastContrast = g_contrast, lastSat = g_saturation;
    int lastVig = g_vignette, lastShadows = g_shadows, lastHighlights = g_highlights;
    float lastClarity = g_clarity, lastClarityRadius = g_clarityRadius;
    float lastWb0 = g_wb[0], lastWb1 = g_wb[1], lastWb2 = g_wb[2];
    float lastCurve0 = g_curveY[0], lastCurve1 = g_curveY[1], lastCurve2 = g_curveY[2];
    float lastCurve3 = g_curveY[3], lastCurve4 = g_curveY[4];

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_BEGIN
#else
    while (!glfwWindowShouldClose(window))
#endif
    {
        if (g_exposure != lastExposure || g_black != lastBlack || g_white != lastWhite ||
            g_contrast != lastContrast || g_saturation != lastSat ||
            g_vignette != lastVig || g_shadows != lastShadows || g_highlights != lastHighlights ||
            g_clarity != lastClarity || g_clarityRadius != lastClarityRadius ||
            g_wb[0] != lastWb0 || g_wb[1] != lastWb1 || g_wb[2] != lastWb2 ||
            g_curveY[0] != lastCurve0 || g_curveY[1] != lastCurve1 ||
            g_curveY[2] != lastCurve2 || g_curveY[3] != lastCurve3 ||
            g_curveY[4] != lastCurve4) {
            MarkToneDirty();
            lastExposure = g_exposure; lastBlack = g_black; lastWhite = g_white;
            lastContrast = g_contrast; lastSat = g_saturation; lastVig = g_vignette;
            lastShadows = g_shadows; lastHighlights = g_highlights;
            lastClarity = g_clarity; lastClarityRadius = g_clarityRadius;
            lastWb0 = g_wb[0]; lastWb1 = g_wb[1]; lastWb2 = g_wb[2];
            lastCurve0 = g_curveY[0]; lastCurve1 = g_curveY[1]; lastCurve2 = g_curveY[2];
            lastCurve3 = g_curveY[3]; lastCurve4 = g_curveY[4];
        }
#ifdef __EMSCRIPTEN__
        if (io.WantSaveIniSettings) { io.WantSaveIniSettings = false; SaveLayoutToStorage(); }
#endif
        RenderFrame(window);
    }
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_END;
#endif

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
#endif // !WASMRAW_SMOKE_TEST
