#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "third_party/space_grotesk_data.h"
#include "tone_common.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "emscripten_mainloop_stub.h"
#endif

struct EditState {
    float exposure = 0.0f;
    int black = 0;
    int white = 100;
    float contrast = 0.0f;
    float saturation = 100.0f;
    int vignette = 0;
    float clarity = 0.0f;
    float clarityRadius = 2.0f;
    int temperature = 0;
    int tint = 0;
    int sharpening = 0;
    float sharpenRadius = 1.0f;
    int denoise = 0;
    int vibrance = 0;
    int ca = 0;
    int distortion = 0;
    int shadows = 0;
    int highlights = 0;
    float wb[3] = { 1.0f, 1.0f, 1.0f };
    int wbPreset = 0;
    float curveY[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    float cropLeft = 0.0f;
    float cropTop = 0.0f;
    float cropRight = 1.0f;
    float cropBottom = 1.0f;
    int rotate = 0;
    int jpegQuality = 90;
    int blurAmount = 0;
    float blurFocusX = 0.5f;
    float blurFocusY = 0.5f;
    float blurRange = 0.35f;
};

struct HistoryEntry {
    char label[64] = {0};
    EditState state;
};

struct Snapshot {
    char name[64] = {0};
    EditState state;
};

struct ImageDocument : EditState {
    int id = 0;
    int revision = 0;
    int renderedRevision = -1;
    int renderedQuality = 0;
    char name[256] = {0};
    bool loaded = false;
    int w = 0, h = 0, colors = 0, bits = 0;
    int pw = 0, ph = 0;
    std::vector<float> rgb;
    std::vector<unsigned char> rendered;
    int renderedWidth = 0;
    int renderedHeight = 0;
    std::vector<unsigned char> before;
    GLuint beforeTexture = 0;
    int beforeWidth = 0;
    int beforeHeight = 0;
    float beforeCropLeft = -1.0f;
    float beforeCropTop = -1.0f;
    float beforeCropRight = -1.0f;
    float beforeCropBottom = -1.0f;
    int beforeRotate = -1;
    // Painted subject-protection mask, normalized to the output image.
    std::vector<unsigned char> mask;
    GLuint maskTexture = 0;
    int maskRevision = 0;
    int maskTextureRevision = -1;
    bool maskEnabled = false;
    int brushSize = 40;
    bool brushErase = false;
    std::vector<unsigned char> thumbnail;
    GLuint thumbnailTexture = 0;
    int thumbnailWidth = 0;
    int thumbnailHeight = 0;
    bool hasFullRes = false;
    bool exportFullRes = true;
    int fw = 0, fh = 0;
    char make[64] = {0};
    char model[64] = {0};
    char lens[80] = {0};
    float iso = 0.f, shutter = 0.f, aperture = 0.f, focal = 0.f;
    int lastError = 0;
    char lastStatus[256] = "No image loaded - drop a RAW file or double-click";
    char exportStatus[160] = "";
    std::vector<HistoryEntry> history;
    int historyPos = -1;
    std::vector<Snapshot> snapshots;
};

static std::deque<ImageDocument> g_images;
static ImageDocument g_empty;
static ImageDocument* g_dec = &g_empty;
static bool g_wbPicking = false;
static bool g_dofPicking = false;
static bool g_maskPainting = false;
static const int kMaskGrid = 256;
static bool g_skipHistory = false;
static bool g_controlHovered = false;
static bool g_showPreview = true;
static bool g_showNavigator = true;
static bool g_showFilmstrip = true;
static bool g_showHistory = true;
static bool g_showToolbox = true;
static bool g_showHistogram = true;
static bool g_showStatus = true;
static bool g_controlChanged = false;
static bool g_beforeAfter = false;
static int g_requestedImageId = 0;

static ImageDocument* FindDocument(int id)
{
    for (ImageDocument& doc : g_images)
        if (doc.id == id) return &doc;
    return nullptr;
}

static bool EditStateEqual(const EditState& a, const EditState& b)
{
    return a.exposure == b.exposure && a.black == b.black && a.white == b.white &&
           a.contrast == b.contrast && a.saturation == b.saturation &&
           a.temperature == b.temperature && a.tint == b.tint &&
           a.vignette == b.vignette && a.clarity == b.clarity &&
           a.clarityRadius == b.clarityRadius && a.sharpening == b.sharpening &&
           a.sharpenRadius == b.sharpenRadius && a.denoise == b.denoise &&
           a.vibrance == b.vibrance && a.ca == b.ca && a.distortion == b.distortion &&
           a.shadows == b.shadows && a.highlights == b.highlights &&
           a.wbPreset == b.wbPreset && a.jpegQuality == b.jpegQuality &&
           a.cropLeft == b.cropLeft && a.cropTop == b.cropTop &&
           a.cropRight == b.cropRight && a.cropBottom == b.cropBottom &&
           a.rotate == b.rotate &&
           a.blurAmount == b.blurAmount && a.blurFocusX == b.blurFocusX &&
           a.blurFocusY == b.blurFocusY && a.blurRange == b.blurRange &&
           a.wb[0] == b.wb[0] && a.wb[1] == b.wb[1] && a.wb[2] == b.wb[2] &&
           a.curveY[0] == b.curveY[0] && a.curveY[1] == b.curveY[1] &&
           a.curveY[2] == b.curveY[2] && a.curveY[3] == b.curveY[3] &&
           a.curveY[4] == b.curveY[4];
}

static void RequestPreview();
static void ToneMapPreview();
static void MarkToneDirty();
static void MarkMaskDirty(ImageDocument* doc);
static void PaintMask(ImageDocument* doc, float u, float v, float radius, bool erase);
static void BuildBeforePreview(ImageDocument* doc);
static bool BeforeNeedsRebuild(const ImageDocument* doc);

static const char* kWbPresets[] = { "Camera", "Daylight", "Cloudy", "Shade",
                                    "Tungsten", "Fluorescent", "Flash", "Custom" };
static const float kWbPresetValues[7][3] = {
    { 1.00f, 1.00f, 1.00f },
    { 1.00f, 1.00f, 1.00f },
    { 0.98f, 0.98f, 1.12f },
    { 0.92f, 1.00f, 1.20f },
    { 0.82f, 1.00f, 1.48f },
    { 1.00f, 1.05f, 1.12f },
    { 1.00f, 1.00f, 1.04f },
};

static int g_histL[256], g_histR[256], g_histG[256], g_histB[256];
static bool g_histDirty = true;
static GLuint g_tex = 0;
static int g_texW = 0, g_texH = 0;
static bool g_previewDirty = true;
static bool g_zoomFit = true;
static float g_zoom = 1.0f;
static ImVec2 g_pan(0.0f, 0.0f);
static ImVec2 g_previewContentSize(0.0f, 0.0f);
static std::vector<unsigned char> g_rgba;
static char g_exportStatus[160] = "";
static const int kPreviewMaxDim = 1600;
static const int kProfileValueCount = 37;
static char g_profileJson[4096] = {0};
static int g_profileJsonLength = 0;

static void ResetPreviewView()
{
    g_zoomFit = true;
    g_zoom = 1.0f;
    g_pan = ImVec2(0.0f, 0.0f);
}

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
    g_texW = w;
    g_texH = h;
}

static void BuildThumbnail(ImageDocument* doc)
{
    if (!doc || !doc->loaded || doc->pw <= 0 || doc->ph <= 0) return;
    const int maxSide = 160;
    const float scale = std::min(1.0f, (float)maxSide / (float)std::max(doc->pw, doc->ph));
    const int width = std::max(1, (int)(doc->pw * scale));
    const int height = std::max(1, (int)(doc->ph * scale));
    std::vector<unsigned char> pixels((size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        int sy = std::clamp((int)((y + 0.5f) / scale), 0, doc->ph - 1);
        for (int x = 0; x < width; ++x) {
            int sx = std::clamp((int)((x + 0.5f) / scale), 0, doc->pw - 1);
            const float* source = &doc->rgb[((size_t)sy * doc->pw + sx) * 3];
            size_t index = ((size_t)y * width + x) * 4;
            pixels[index + 0] = (unsigned char)(tone::SrgbEncode(source[0]) * 255.0f + 0.5f);
            pixels[index + 1] = (unsigned char)(tone::SrgbEncode(source[1]) * 255.0f + 0.5f);
            pixels[index + 2] = (unsigned char)(tone::SrgbEncode(source[2]) * 255.0f + 0.5f);
            pixels[index + 3] = 255;
        }
    }
    if (doc->thumbnailTexture) glDeleteTextures(1, &doc->thumbnailTexture);
    glGenTextures(1, &doc->thumbnailTexture);
    glBindTexture(GL_TEXTURE_2D, doc->thumbnailTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    doc->thumbnail = std::move(pixels);
    doc->thumbnailWidth = width;
    doc->thumbnailHeight = height;
}

static void InitializeHistory(ImageDocument* doc)
{
    if (!doc || !doc->history.empty()) return;
    HistoryEntry entry;
    snprintf(entry.label, sizeof(entry.label), "Open");
    entry.state = *doc;
    doc->history.push_back(entry);
    doc->historyPos = 0;
}

static void RecordHistory(ImageDocument* doc, const char* label)
{
    if (!doc) return;
    InitializeHistory(doc);
    if (doc->historyPos >= 0 && EditStateEqual(doc->history[doc->historyPos].state, *doc)) return;
    if (doc->historyPos + 1 < (int)doc->history.size())
        doc->history.resize(doc->historyPos + 1);
    HistoryEntry entry;
    snprintf(entry.label, sizeof(entry.label), "%s", label ? label : "Edit");
    entry.state = *doc;
    doc->history.push_back(entry);
    doc->historyPos = (int)doc->history.size() - 1;
}

static void ApplyEditState(const EditState& state)
{
    if (!g_dec) return;
    static_cast<EditState&>(*g_dec) = state;
    g_skipHistory = true;
    g_dec->revision++;
    g_previewDirty = true;
    g_histDirty = true;
    RequestPreview();
}

static void UndoEdit()
{
    if (!g_dec || g_dec->historyPos <= 0) return;
    g_dec->historyPos--;
    ApplyEditState(g_dec->history[g_dec->historyPos].state);
}

static void RedoEdit()
{
    if (!g_dec || g_dec->historyPos + 1 >= (int)g_dec->history.size()) return;
    g_dec->historyPos++;
    ApplyEditState(g_dec->history[g_dec->historyPos].state);
}

static void AddSnapshot()
{
    if (!g_dec) return;
    Snapshot snapshot;
    snprintf(snapshot.name, sizeof(snapshot.name), "Snapshot %d", (int)g_dec->snapshots.size() + 1);
    snapshot.state = *g_dec;
    g_dec->snapshots.push_back(snapshot);
}

static void ActivateDocument(ImageDocument* doc)
{
    if (!doc) return;
    g_dec = doc;
    g_requestedImageId = doc->id;
    if (doc->loaded && !doc->rendered.empty() && doc->renderedWidth > 0 && doc->renderedHeight > 0)
        g_rgba = doc->rendered;
    else if (doc->loaded && !doc->rgb.empty()) {
        g_rgba.clear();
        ToneMapPreview();
    } else
        g_rgba.clear();
    g_texW = 0;
    g_texH = 0;
    ResetPreviewView();
    g_previewDirty = true;
    g_histDirty = true;
}

static tone::Params CurrentToneParams()
{
    tone::Params p;
    p.exposure = g_dec->exposure;
    p.black = (float)g_dec->black / 100.0f;
    p.white = (float)g_dec->white / 100.0f;
    p.contrast = g_dec->contrast / 100.0f;
    p.saturation = g_dec->saturation / 100.0f;
    p.temperature = (float)g_dec->temperature / 100.0f;
    p.tint = (float)g_dec->tint / 100.0f;
    p.vignette = (float)g_dec->vignette / 100.0f;
    p.clarity = g_dec->clarity / 100.0f;
    p.clarityRadius = g_dec->clarityRadius;
    p.sharpening = (float)g_dec->sharpening / 100.0f;
    p.sharpenRadius = g_dec->sharpenRadius;
    p.denoise = (float)g_dec->denoise / 100.0f;
    p.vibrance = (float)g_dec->vibrance / 100.0f;
    p.ca = (float)g_dec->ca / 100.0f;
    p.distortion = (float)g_dec->distortion / 100.0f;
    p.shadows = (float)g_dec->shadows / 100.0f;
    p.highlights = (float)g_dec->highlights / 100.0f;
    p.wbR = g_dec->wb[0]; p.wbG = g_dec->wb[1]; p.wbB = g_dec->wb[2];
    for (int i = 0; i < 5; ++i) p.curveY[i] = g_dec->curveY[i];
    p.cropLeft = g_dec->cropLeft;
    p.cropTop = g_dec->cropTop;
    p.cropRight = g_dec->cropRight;
    p.cropBottom = g_dec->cropBottom;
    p.rotate = g_dec->rotate;
    p.blurAmount = (float)g_dec->blurAmount / 100.0f;
    p.blurFocusX = g_dec->blurFocusX;
    p.blurFocusY = g_dec->blurFocusY;
    p.blurRange = g_dec->blurRange;
    if (g_dec->maskEnabled && !g_dec->mask.empty()) {
        p.mask = g_dec->mask.data();
        p.maskSize = kMaskGrid;
    } else {
        p.mask = nullptr;
        p.maskSize = 0;
    }
    return p;
}

static void ToneMapPreview()
{
    if (!g_dec->loaded || g_dec->pw <= 0 || g_dec->ph <= 0) return;
    tone::Params params = CurrentToneParams();
    tone::ToneMapToBuffer(g_dec->rgb, g_dec->pw, g_dec->ph, params, g_rgba);
    if (!g_rgba.empty()) {
        tone::GetOutputSize(g_dec->pw, g_dec->ph, params,
                            g_dec->renderedWidth, g_dec->renderedHeight);
        g_dec->rendered = g_rgba;
        g_dec->renderedQuality = 0;
        g_dec->renderedRevision = g_dec->revision;
        if (g_beforeAfter && BeforeNeedsRebuild(g_dec)) BuildBeforePreview(g_dec);
    }
}

static void BuildBeforePreview(ImageDocument* doc)
{
    if (!doc || !doc->loaded || doc->pw <= 0 || doc->ph <= 0) return;
    tone::Params params;
    params.cropLeft = doc->cropLeft;
    params.cropTop = doc->cropTop;
    params.cropRight = doc->cropRight;
    params.cropBottom = doc->cropBottom;
    params.rotate = doc->rotate;
    std::vector<unsigned char> pixels;
    tone::ToneMapToBuffer(doc->rgb, doc->pw, doc->ph, params, pixels);
    int width, height;
    tone::GetOutputSize(doc->pw, doc->ph, params, width, height);
    if (doc->beforeTexture) glDeleteTextures(1, &doc->beforeTexture);
    glGenTextures(1, &doc->beforeTexture);
    glBindTexture(GL_TEXTURE_2D, doc->beforeTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    doc->before = std::move(pixels);
    doc->beforeWidth = width;
    doc->beforeHeight = height;
    doc->beforeCropLeft = doc->cropLeft;
    doc->beforeCropTop = doc->cropTop;
    doc->beforeCropRight = doc->cropRight;
    doc->beforeCropBottom = doc->cropBottom;
    doc->beforeRotate = doc->rotate;
}

// The unedited reference only depends on geometry, so it must not be rebuilt on
// every slider tick. That is pure waste and shows up as frame stalls.
static bool BeforeNeedsRebuild(const ImageDocument* doc)
{
    return doc->beforeTexture == 0 || doc->beforeWidth != doc->renderedWidth ||
           doc->beforeHeight != doc->renderedHeight || doc->beforeRotate != doc->rotate ||
           doc->beforeCropLeft != doc->cropLeft || doc->beforeCropTop != doc->cropTop ||
           doc->beforeCropRight != doc->cropRight || doc->beforeCropBottom != doc->cropBottom;
}

static void MapOutputToSource(int outputX, int outputY, int& sourceX, int& sourceY)
{
    tone::Params params = CurrentToneParams();
    int left, top, right, bottom;
    tone::GetCropBounds(g_dec->pw, g_dec->ph, params, left, top, right, bottom);
    int cropW = std::max(1, right - left);
    int cropH = std::max(1, bottom - top);
    int rotation = ((g_dec->rotate % 360) + 360) % 360;
    int cropX = outputX;
    int cropY = outputY;
    if (rotation == 90) {
        cropX = outputY;
        cropY = cropW - 1 - outputX;
    } else if (rotation == 180) {
        cropX = cropW - 1 - outputX;
        cropY = cropH - 1 - outputY;
    } else if (rotation == 270) {
        cropX = cropH - 1 - outputY;
        cropY = outputX;
    }
    sourceX = std::clamp(left + cropX, 0, g_dec->pw - 1);
    sourceY = std::clamp(top + cropY, 0, g_dec->ph - 1);
}

static void RebuildPreview()
{
#ifndef __EMSCRIPTEN__
    ToneMapPreview();
#endif
    const int w = g_dec->renderedWidth > 0 ? g_dec->renderedWidth : g_dec->pw;
    const int h = g_dec->renderedHeight > 0 ? g_dec->renderedHeight : g_dec->ph;
    if (w <= 0 || h <= 0 || g_rgba.empty()) return;
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

#ifdef __EMSCRIPTEN__
EM_JS(void, js_request_preview, (int imageId, int revision,
                                  double exp, double black, double white,
                                  double contrast, double sat, double temperature, double tint,
                                  double wbR, double wbG, double wbB, double vignette,
                                  double clarity, double clarityRadius, double sharpening,
                                  double sharpenRadius, double denoise, double vibrance,
                                  double ca, double distortion, double shadows, double highlights,
                                  double cy0, double cy1, double cy2, double cy3, double cy4,
                                  double cropLeft, double cropTop, double cropRight, double cropBottom,
                                  int rotate, int quality,
                                  double blurAmount, double blurFocusX, double blurFocusY,
                                  double blurRange,
                                  const unsigned char* mask, int maskSize), {
    if (!window._wasmraw || !window._wasmraw.requestPreview) return;
    window._wasmraw.requestPreview(imageId, revision, exp, black, white, contrast, sat,
        temperature, tint, wbR, wbG, wbB, vignette, clarity, clarityRadius,
        sharpening, sharpenRadius, denoise, vibrance, ca, distortion, shadows, highlights,
        cy0, cy1, cy2, cy3, cy4, cropLeft, cropTop, cropRight, cropBottom, rotate, quality,
        blurAmount, blurFocusX, blurFocusY, blurRange, mask, maskSize);
});
#endif

static void RequestPreview()
{
    if (!g_dec || !g_dec->loaded) return;
#ifdef __EMSCRIPTEN__
    js_request_preview(g_dec->id, g_dec->revision,
                       g_dec->exposure,
                       (double)g_dec->black / 100.0,
                       (double)g_dec->white / 100.0,
                       (double)g_dec->contrast / 100.0,
                       (double)g_dec->saturation / 100.0,
                       (double)g_dec->temperature / 100.0,
                       (double)g_dec->tint / 100.0,
                       g_dec->wb[0], g_dec->wb[1], g_dec->wb[2],
                       (double)g_dec->vignette / 100.0,
                       (double)g_dec->clarity / 100.0,
                       g_dec->clarityRadius,
                       (double)g_dec->sharpening / 100.0,
                       g_dec->sharpenRadius,
                       (double)g_dec->denoise / 100.0,
                       (double)g_dec->vibrance / 100.0,
                       (double)g_dec->ca / 100.0,
                       (double)g_dec->distortion / 100.0,
                       (double)g_dec->shadows / 100.0,
                       (double)g_dec->highlights / 100.0,
                       g_dec->curveY[0], g_dec->curveY[1], g_dec->curveY[2], g_dec->curveY[3], g_dec->curveY[4],
                       g_dec->cropLeft, g_dec->cropTop, g_dec->cropRight, g_dec->cropBottom,
                       g_dec->rotate, 0,
                       (double)g_dec->blurAmount / 100.0,
                       g_dec->blurFocusX, g_dec->blurFocusY, g_dec->blurRange,
                       (g_dec->maskEnabled && !g_dec->mask.empty()) ? g_dec->mask.data() : nullptr,
                       g_dec->maskEnabled ? (int)g_dec->mask.size() : 0);
#else
    g_previewDirty = true;
#endif
}

// ---------- WASM API for the JS shell ----------
// Decode and full-resolution export run in the decoder worker. This module
// receives the preview floats + metadata the worker produced, and forwards
// export requests / results to/from the worker.

extern "C" {

EMSCRIPTEN_KEEPALIVE
void wasm_add_image(int imageId, const char* name)
{
    if (imageId <= 0) return;
    ImageDocument* doc = FindDocument(imageId);
    if (doc) {
        if (name && name[0]) snprintf(doc->name, sizeof(doc->name), "%s", name);
        return;
    }
    g_images.emplace_back();
    doc = &g_images.back();
    doc->id = imageId;
    if (name) snprintf(doc->name, sizeof(doc->name), "%s", name);
    snprintf(doc->lastStatus, sizeof(doc->lastStatus), "Queued");
    InitializeHistory(doc);
    if (g_dec == &g_empty) ActivateDocument(doc);
}

EMSCRIPTEN_KEEPALIVE
void wasm_select_image(int imageId)
{
    ImageDocument* doc = FindDocument(imageId);
    if (doc) {
        ActivateDocument(doc);
        if (doc->loaded) {
            if (g_beforeAfter) BuildBeforePreview(doc);
            RequestPreview();
        }
    }
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_image_status(int imageId, const char* text)
{
    ImageDocument* doc = FindDocument(imageId);
    if (doc) snprintf(doc->lastStatus, sizeof(doc->lastStatus), "%s", text ? text : "");
}

EMSCRIPTEN_KEEPALIVE
int wasm_control_hovered() { return g_controlHovered ? 1 : 0; }

// ---------- UI font ----------
// The atlas holds two sizes of the same typeface: one for panels and controls,
// one for the menu bar. Rebuilding it mid-frame is illegal, so a change only
// flags the work and RenderFrame applies it before the next NewFrame.
static std::vector<unsigned char> g_fontData;
static std::string g_fontName = "Default";
static float g_fontSize = 15.0f;
static float g_menuFontSize = 16.0f;
static bool g_fontDirty = true;
static ImFont* g_uiFont = nullptr;
static ImFont* g_menuFont = nullptr;
static bool g_lightTheme = false;
static bool g_showFps = false;
static bool g_themeDirty = true;
static bool g_layoutResetPending = false;

// Interface settings live in the Settings menu and are mirrored to
// localStorage, so a reload keeps the look the user picked.
static void ApplyTheme()
{
    if (g_lightTheme) ImGui::StyleColorsLight();
    else ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
}

// Space Grotesk backs the menu bar. It is bundled as a byte array so the build
// stays self-contained; see assets/fonts/OFL.txt for the licence.
static_assert(sizeof(kSpaceGrotesk) == kSpaceGroteskSize,
              "embedded font data does not match its declared size");

static ImFont* AddBundledFont(ImFontAtlas* atlas, float pixelSize)
{
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 1;
    config.OversampleV = 1;
    return atlas->AddFontFromMemoryTTF((void*)kSpaceGrotesk, (int)kSpaceGroteskSize,
                                       pixelSize, &config);
}

static void ApplyFontAtlas()
{
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->ClearFonts();
    g_uiFont = nullptr;
    g_menuFont = nullptr;

    if (!g_fontData.empty()) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 1;
        config.OversampleV = 1;
        g_uiFont = atlas->AddFontFromMemoryTTF(g_fontData.data(), (int)g_fontData.size(),
                                                g_fontSize, &config);
    } else {
        ImFontConfig uiConfig;
        uiConfig.SizePixels = g_fontSize;
        g_uiFont = atlas->AddFontDefault(&uiConfig);
    }
    g_menuFont = AddBundledFont(atlas, g_menuFontSize);
    g_fontDirty = false;
}

EMSCRIPTEN_KEEPALIVE
int wasm_load_font(const unsigned char* data, int size)
{
    if (!data || size <= 0) return 0;
    try {
        g_fontData.assign(data, data + size);
    } catch (const std::bad_alloc&) {
        g_fontData.clear();
        return 0;
    }
    g_fontDirty = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_font_name(const char* name)
{
    g_fontName = name && name[0] ? name : "Custom";
}

EMSCRIPTEN_KEEPALIVE
int wasm_reset_font()
{
    g_fontData.clear();
    g_fontName = "Default";
    g_fontDirty = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_font_sizes(float ui, float menu)
{
    g_fontSize = std::clamp(ui, 8.0f, 48.0f);
    g_menuFontSize = std::clamp(menu, 8.0f, 48.0f);
    g_fontDirty = true;
}

EMSCRIPTEN_KEEPALIVE
void wasm_apply_ui_settings(int lightTheme, double fontSize, double menuFontSize, int showFps)
{
    g_lightTheme = lightTheme != 0;
    g_fontSize = std::clamp((float)fontSize, 8.0f, 48.0f);
    g_menuFontSize = std::clamp((float)menuFontSize, 8.0f, 48.0f);
    g_showFps = showFps != 0;
    g_themeDirty = true;
    g_fontDirty = true;
}

EMSCRIPTEN_KEEPALIVE
void wasm_accept_decode(int imageId, const float* preview, int w, int h, int pw, int ph,
                        double iso, double shutter, double aperture, double focal,
                        const char* make, const char* model, const char* lens,
                        const char* status, int hasFullRes)
{
    ImageDocument* doc = FindDocument(imageId);
    if (!doc) {
        wasm_add_image(imageId, "");
        doc = FindDocument(imageId);
    }
    if (!doc || !preview) return;
    bool activate = (g_dec == &g_empty || g_dec->id == imageId);
    if (activate) {
        g_dec = doc;
        g_requestedImageId = imageId;
        g_rgba.clear();
        g_texW = 0;
        g_texH = 0;
        ResetPreviewView();
    }
    doc->loaded = true;
    doc->w = w; doc->h = h;
    doc->pw = pw; doc->ph = ph;
    doc->colors = 3;
    doc->bits = 16;
    doc->hasFullRes = hasFullRes != 0;
    if (!doc->hasFullRes) doc->exportFullRes = false;
    doc->fw = w; doc->fh = h;
    doc->iso = (float)iso;
    doc->shutter = (float)shutter;
    doc->aperture = (float)aperture;
    doc->focal = (float)focal;
    snprintf(doc->make, sizeof(doc->make), "%s", make ? make : "");
    snprintf(doc->model, sizeof(doc->model), "%s", model ? model : "");
    snprintf(doc->lens, sizeof(doc->lens), "%s", lens ? lens : "");
    snprintf(doc->lastStatus, sizeof(doc->lastStatus), "%s", status ? status : "Decoded");
    doc->rgb.assign(preview, preview + (size_t)pw * ph * 3);
    doc->rendered.clear();
    doc->renderedWidth = 0;
    doc->renderedHeight = 0;
    BuildThumbnail(doc);
    doc->lastError = 0;
    InitializeHistory(doc);
    if (activate) {
        ToneMapPreview();
        g_previewDirty = true;
        g_histDirty = true;
        RequestPreview();
    }
}

EMSCRIPTEN_KEEPALIVE
void wasm_accept_preview(int imageId, const unsigned char* rgba, int w, int h,
                         int quality, int revision)
{
    ImageDocument* doc = FindDocument(imageId);
    if (!doc || !rgba || w <= 0 || h <= 0) return;
    doc->rendered.assign(rgba, rgba + (size_t)w * h * 4);
    doc->renderedWidth = w;
    doc->renderedHeight = h;
    doc->renderedRevision = revision;
    doc->renderedQuality = quality;
    if (doc == g_dec || imageId == g_requestedImageId) {
        g_dec = doc;
        g_requestedImageId = imageId;
        g_rgba = doc->rendered;
        g_texW = 0;
        g_texH = 0;
        g_previewDirty = true;
        g_histDirty = true;
        if (g_beforeAfter && BeforeNeedsRebuild(doc)) BuildBeforePreview(doc);
    }
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_export_status(int imageId, const char* text)
{
    snprintf(g_exportStatus, sizeof(g_exportStatus), "%s", text ? text : "");
    ImageDocument* doc = FindDocument(imageId);
    if (doc) snprintf(doc->exportStatus, sizeof(doc->exportStatus), "%s", text ? text : "");
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_get_profile_json()
{
    if (!g_dec || g_dec->id == 0) {
        g_profileJsonLength = 2;
        memcpy(g_profileJson, "{}", 3);
        return g_profileJson;
    }
    std::string json = "{\"version\":1";
    auto add = [&](const char* name, double value) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), ",\"%s\":%.9g", name, value);
        json += buffer;
    };
    add("exposure", g_dec->exposure);
    add("black", g_dec->black);
    add("white", g_dec->white);
    add("contrast", g_dec->contrast);
    add("saturation", g_dec->saturation);
    add("vignette", g_dec->vignette);
    add("clarity", g_dec->clarity);
    add("clarityRadius", g_dec->clarityRadius);
    add("temperature", g_dec->temperature);
    add("tint", g_dec->tint);
    add("sharpening", g_dec->sharpening);
    add("sharpenRadius", g_dec->sharpenRadius);
    add("denoise", g_dec->denoise);
    add("vibrance", g_dec->vibrance);
    add("ca", g_dec->ca);
    add("distortion", g_dec->distortion);
    add("shadows", g_dec->shadows);
    add("highlights", g_dec->highlights);
    add("wbR", g_dec->wb[0]);
    add("wbG", g_dec->wb[1]);
    add("wbB", g_dec->wb[2]);
    add("wbPreset", g_dec->wbPreset);
    add("curve0", g_dec->curveY[0]);
    add("curve1", g_dec->curveY[1]);
    add("curve2", g_dec->curveY[2]);
    add("curve3", g_dec->curveY[3]);
    add("curve4", g_dec->curveY[4]);
    add("cropLeft", g_dec->cropLeft);
    add("cropTop", g_dec->cropTop);
    add("cropRight", g_dec->cropRight);
    add("cropBottom", g_dec->cropBottom);
    add("rotate", g_dec->rotate);
    add("jpegQuality", g_dec->jpegQuality);
    add("blurAmount", g_dec->blurAmount);
    add("blurFocusX", g_dec->blurFocusX);
    add("blurFocusY", g_dec->blurFocusY);
    add("blurRange", g_dec->blurRange);
    json += "}";
    g_profileJsonLength = (int)json.size();
    snprintf(g_profileJson, sizeof(g_profileJson), "%s", json.c_str());
    return g_profileJson;
}

EMSCRIPTEN_KEEPALIVE
int wasm_profile_json_length() { return g_profileJsonLength; }

EMSCRIPTEN_KEEPALIVE
int wasm_apply_profile_values(const double* values, int count)
{
    if (!g_dec || g_dec->id == 0 || !values || count != kProfileValueCount) return 0;
    EditState state;
    int i = 0;
    state.exposure = (float)values[i++];
    state.black = (int)values[i++];
    state.white = (int)values[i++];
    state.contrast = (float)values[i++];
    state.saturation = (float)values[i++];
    state.vignette = (int)values[i++];
    state.clarity = (float)values[i++];
    state.clarityRadius = (float)values[i++];
    state.temperature = (int)values[i++];
    state.tint = (int)values[i++];
    state.sharpening = (int)values[i++];
    state.sharpenRadius = (float)values[i++];
    state.denoise = (int)values[i++];
    state.vibrance = (int)values[i++];
    state.ca = (int)values[i++];
    state.distortion = (int)values[i++];
    state.shadows = (int)values[i++];
    state.highlights = (int)values[i++];
    state.wb[0] = (float)values[i++];
    state.wb[1] = (float)values[i++];
    state.wb[2] = (float)values[i++];
    state.wbPreset = (int)values[i++];
    for (int curve = 0; curve < 5; ++curve) state.curveY[curve] = (float)values[i++];
    state.cropLeft = (float)values[i++];
    state.cropTop = (float)values[i++];
    state.cropRight = (float)values[i++];
    state.cropBottom = (float)values[i++];
    state.rotate = (int)values[i++];
    state.jpegQuality = (int)values[i++];
    state.blurAmount = std::clamp((int)values[i++], 0, 100);
    state.blurFocusX = std::clamp((float)values[i++], 0.0f, 1.0f);
    state.blurFocusY = std::clamp((float)values[i++], 0.0f, 1.0f);
    state.blurRange = std::clamp((float)values[i++], 0.05f, 1.0f);
    if (i != kProfileValueCount) return 0;
    ApplyEditState(state);
#ifdef __EMSCRIPTEN__
    ToneMapPreview();
    g_histDirty = true;
#endif
    return 1;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_request_export, (int imageId, int revision, int fmt, int fullRes, double exp, double black, double white,
                                double contrast, double sat, double temperature, double tint,
                                double wbR, double wbG, double wbB, double vignette,
                                double clarity, double clarityRadius, double sharpening,
                                double sharpenRadius, double denoise, double vibrance,
                                double ca, double distortion, double shadows, double highlights,
                                double cy0, double cy1, double cy2, double cy3, double cy4,
                                double cropLeft, double cropTop, double cropRight, double cropBottom,
                                int rotate, double quality,
                                double blurAmount, double blurFocusX, double blurFocusY,
                                double blurRange,
                                const unsigned char* mask, int maskSize), {
    if (!window._wasmraw || !window._wasmraw.requestExport) return;
    window._wasmraw.requestExport(imageId, revision, fmt, fullRes, exp, black, white,
        contrast, sat, temperature, tint, wbR, wbG, wbB, vignette, clarity, clarityRadius,
        sharpening, sharpenRadius, denoise, vibrance, ca, distortion, shadows, highlights,
        cy0, cy1, cy2, cy3, cy4, cropLeft, cropTop, cropRight, cropBottom, rotate,
        Math.floor(quality), blurAmount, blurFocusX, blurFocusY, blurRange, mask, maskSize);
});

EM_JS(void, js_select_image, (int imageId), {
    if (window._wasmraw && window._wasmraw.select) window._wasmraw.select(imageId);
});

EM_JS(void, js_save_profile, (), {
    if (window._wasmraw && window._wasmraw.saveProfile) window._wasmraw.saveProfile();
});

EM_JS(void, js_load_profile, (), {
    if (window._wasmraw && window._wasmraw.loadProfile) window._wasmraw.loadProfile();
});

EM_JS(void, js_open_file_dialog, (), {
    if (window._wasmraw && window._wasmraw.openFileDialog) window._wasmraw.openFileDialog();
});

EM_JS(void, js_open_font_dialog, (), {
    if (window._wasmraw && window._wasmraw.openFontDialog) window._wasmraw.openFontDialog();
});

EM_JS(void, js_forget_font, (), {
    if (window._wasmraw && window._wasmraw.forgetFont) window._wasmraw.forgetFont();
});

EM_JS(void, js_forget_layout, (), {
    if (window._wasmraw && window._wasmraw.forgetLayout) window._wasmraw.forgetLayout();
});
#endif

static void RequestExport(int format)
{
    if (!g_dec->loaded) return;
    snprintf(g_exportStatus, sizeof(g_exportStatus), "Exporting...");
    snprintf(g_dec->exportStatus, sizeof(g_dec->exportStatus), "Exporting...");
#ifdef __EMSCRIPTEN__
    js_request_export(g_dec->id, g_dec->revision, format, g_dec->exportFullRes,
                      g_dec->exposure,
                      (double)g_dec->black / 100.0,
                      (double)g_dec->white / 100.0,
                      (double)g_dec->contrast / 100.0,
                      (double)g_dec->saturation / 100.0,
                      (double)g_dec->temperature / 100.0,
                      (double)g_dec->tint / 100.0,
                      g_dec->wb[0], g_dec->wb[1], g_dec->wb[2],
                      (double)g_dec->vignette / 100.0,
                      (double)g_dec->clarity / 100.0,
                      g_dec->clarityRadius,
                      (double)g_dec->sharpening / 100.0,
                      g_dec->sharpenRadius,
                      (double)g_dec->denoise / 100.0,
                      (double)g_dec->vibrance / 100.0,
                      (double)g_dec->ca / 100.0,
                      (double)g_dec->distortion / 100.0,
                      (double)g_dec->shadows / 100.0,
                      (double)g_dec->highlights / 100.0,
                      g_dec->curveY[0], g_dec->curveY[1], g_dec->curveY[2], g_dec->curveY[3], g_dec->curveY[4],
                      g_dec->cropLeft, g_dec->cropTop, g_dec->cropRight, g_dec->cropBottom,
                      g_dec->rotate, g_dec->jpegQuality,
                      (double)g_dec->blurAmount / 100.0,
                      g_dec->blurFocusX, g_dec->blurFocusY, g_dec->blurRange,
                      (g_dec->maskEnabled && !g_dec->mask.empty()) ? g_dec->mask.data() : nullptr,
                      g_dec->maskEnabled ? (int)g_dec->mask.size() : 0);
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

EM_JS(void, js_set_ui_settings, (const char* src), {
    try {
        localStorage.setItem('wasmraw.settings', UTF8ToString(src));
    } catch (e) { /* quota / privacy mode: ignore */ }
});

EM_JS(int, js_get_ui_settings, (char* dst, int maxlen), {
    try {
        var s = localStorage.getItem('wasmraw.settings');
        if (!s) return 0;
        if (s.length >= maxlen) s = s.substr(0, maxlen - 1);
        stringToUTF8(s, dst, maxlen);
        return s.length;
    } catch (e) { return 0; }
});

static void SaveUiSettings()
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"version\":1,\"lightTheme\":%d,\"fontSize\":%.1f,\"menuFontSize\":%.1f,"
             "\"showFps\":%d}",
             g_lightTheme ? 1 : 0, g_fontSize, g_menuFontSize, g_showFps ? 1 : 0);
    js_set_ui_settings(json);
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
    if (!g_dec->loaded || g_rgba.empty()) return;
    memset(g_histL, 0, sizeof(g_histL));
    memset(g_histR, 0, sizeof(g_histR));
    memset(g_histG, 0, sizeof(g_histG));
    memset(g_histB, 0, sizeof(g_histB));
    const unsigned char* s = g_rgba.data();
    int renderedW = g_dec->renderedWidth > 0 ? g_dec->renderedWidth : g_dec->pw;
    int renderedH = g_dec->renderedHeight > 0 ? g_dec->renderedHeight : g_dec->ph;
    size_t n = (size_t)renderedW * renderedH;
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
    ImGui::Begin("Histogram", &g_showHistogram);
    if (!g_dec->loaded || g_rgba.empty()) {
        if (g_dec->id > 0 && !g_dec->loaded)
            ImGui::TextDisabled("Loading %s", g_dec->lastStatus);
        else
            ImGui::TextDisabled(g_dec->loaded ? "Preparing preview..." : "No image loaded");
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

static void DrawNavigator()
{
    ImGui::Begin("Navigator", &g_showNavigator);
    if (g_dec->id > 0 && !g_dec->loaded) {
        ImGui::TextDisabled("Loading %s", g_dec->lastStatus);
        ImGui::End();
        return;
    }
    if (!g_dec->loaded || !g_tex || g_texW <= 0 || g_texH <= 0) {
        ImGui::TextDisabled(g_dec->loaded ? "Preparing preview..." : "No image loaded");
        ImGui::End();
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 40.0f || avail.y < 40.0f) {
        ImGui::End();
        return;
    }
    const float imageWidth = (float)g_texW;
    const float imageHeight = (float)g_texH;
    const float scale = std::min(avail.x / imageWidth, avail.y / imageHeight);
    const ImVec2 shown(imageWidth * scale, imageHeight * scale);
    ImGui::InvisibleButton("navigator_view", shown);
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddImage((ImTextureID)(intptr_t)g_tex, origin, ImVec2(origin.x + shown.x, origin.y + shown.y));

    float viewScale = g_zoomFit
        ? std::min((g_previewContentSize.x > 0.f ? g_previewContentSize.x : avail.x) / imageWidth,
                   (g_previewContentSize.y > 0.f ? g_previewContentSize.y : avail.y) / imageHeight)
        : g_zoom;
    viewScale = std::max(0.05f, viewScale);
    const float visibleWidth = std::min(imageWidth, (g_previewContentSize.x > 0.f ? g_previewContentSize.x : avail.x) / viewScale);
    const float visibleHeight = std::min(imageHeight, (g_previewContentSize.y > 0.f ? g_previewContentSize.y : avail.y) / viewScale);
    const float centerX = imageWidth * 0.5f - g_pan.x / viewScale;
    const float centerY = imageHeight * 0.5f - g_pan.y / viewScale;
    const float x0 = std::max(0.0f, centerX - visibleWidth * 0.5f);
    const float y0 = std::max(0.0f, centerY - visibleHeight * 0.5f);
    const float x1 = std::min(imageWidth, centerX + visibleWidth * 0.5f);
    const float y1 = std::min(imageHeight, centerY + visibleHeight * 0.5f);
    drawList->AddRect(ImVec2(origin.x + x0 * scale, origin.y + y0 * scale),
                      ImVec2(origin.x + x1 * scale, origin.y + y1 * scale),
                      IM_COL32(255, 180, 80, 220), 0, 1.5f);

    if (hovered) {
        float u = (mouse.x - origin.x) / shown.x;
        float v = (mouse.y - origin.y) / shown.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            int outputX = std::clamp((int)(u * (g_texW - 1)), 0, g_texW - 1);
            int outputY = std::clamp((int)(v * (g_texH - 1)), 0, g_texH - 1);
            int px, py;
            MapOutputToSource(outputX, outputY, px, py);
            const float* pixel = &g_dec->rgb[((size_t)py * g_dec->pw + px) * 3];
            ImGui::Text("Pixel %d, %d", px, py);
            ImGui::Text("RGB %.3f %.3f %.3f", pixel[0], pixel[1], pixel[2]);
            if (ImGui::IsMouseClicked(0)) {
                g_zoomFit = false;
                g_zoom = viewScale;
                g_pan.x = (outputX + 0.5f - imageWidth * 0.5f) * viewScale;
                g_pan.y = (outputY + 0.5f - imageHeight * 0.5f) * viewScale;
            }
        }
    }
    ImGui::End();
}

static void DrawHistory()
{
    ImGui::Begin("History", &g_showHistory);
    if (!g_dec || g_dec->id == 0) {
        ImGui::TextDisabled("No image loaded");
        ImGui::End();
        return;
    }
    if (ImGui::Button("Undo")) UndoEdit();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) RedoEdit();
    ImGui::SameLine();
    if (ImGui::Button("Snapshot")) AddSnapshot();
    ImGui::Separator();
    ImGui::Text("Edits");
    for (int i = 0; i < (int)g_dec->history.size(); ++i) {
        bool selected = i == g_dec->historyPos;
        if (ImGui::Selectable(g_dec->history[i].label, selected) && !selected) {
            g_dec->historyPos = i;
            ApplyEditState(g_dec->history[i].state);
        }
    }
    if (!g_dec->snapshots.empty()) {
        ImGui::Separator();
        ImGui::Text("Snapshots");
        for (const Snapshot& snapshot : g_dec->snapshots) {
            if (ImGui::Selectable(snapshot.name, false))
                ApplyEditState(snapshot.state);
        }
    }
#ifdef __EMSCRIPTEN__
    ImGui::Separator();
    if (ImGui::Button("Save profile")) js_save_profile();
    ImGui::SameLine();
    if (ImGui::Button("Load profile")) js_load_profile();
#endif
    ImGui::End();
}

static void SelectDocument(ImageDocument* doc)
{
    if (!doc) return;
    ActivateDocument(doc);
#ifdef __EMSCRIPTEN__
    js_select_image(doc->id);
#endif
}

static void DrawFilmstrip()
{
    ImGui::SetNextWindowSize(ImVec2(220.0f, 420.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Filmstrip", &g_showFilmstrip);
    if (g_images.empty()) {
        ImGui::TextDisabled("No images loaded");
        ImGui::End();
        return;
    }
    for (ImageDocument& doc : g_images) {
        ImGui::PushID(doc.id);
        bool clicked = false;
        if (doc.thumbnailTexture)
            clicked = ImGui::ImageButton("##thumbnail", (ImTextureID)(intptr_t)doc.thumbnailTexture,
                                         ImVec2(128.0f, 80.0f));
        else
            clicked = ImGui::Button("No preview", ImVec2(128.0f, 80.0f));
        if (clicked) SelectDocument(&doc);
        bool selected = &doc == g_dec;
        if (ImGui::Selectable(doc.name[0] ? doc.name : "Unnamed image", selected))
            SelectDocument(&doc);
        ImGui::TextDisabled("%s", doc.lastStatus);
        ImGui::PopID();
    }
    ImGui::End();
}

static bool SliderFloatWithReset(const char* label, float* value, float minValue, float maxValue,
                                 const char* format, float resetValue)
{
    ImGui::PushID(label);
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    const float resetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(40.0f, available - labelWidth - resetWidth - spacing * 2.0f));
    bool changed = ImGui::SliderFloat("##value", value, minValue, maxValue, format);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemActive())
        g_controlHovered = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        *value = resetValue;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        g_controlHovered = true;
    if (changed) {
        MarkToneDirty();
        g_controlChanged = true;
    }
    ImGui::PopID();
    return changed;
}

static bool SliderIntWithReset(const char* label, int* value, int minValue, int maxValue,
                               const char* format, int resetValue)
{
    ImGui::PushID(label);
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    const float resetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(40.0f, available - labelWidth - resetWidth - spacing * 2.0f));
    bool changed = ImGui::SliderInt("##value", value, minValue, maxValue, format);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemActive())
        g_controlHovered = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        *value = resetValue;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        g_controlHovered = true;
    if (changed) {
        MarkToneDirty();
        g_controlChanged = true;
    }
    ImGui::PopID();
    return changed;
}

static void DrawToolbox()
{
    ImGui::Begin("Toolbox", &g_showToolbox);
    SliderFloatWithReset("Exposure", &g_dec->exposure, -5.0f, 5.0f, "%.2f EV", 0.0f);
    ImGui::TextUnformatted("Black / White points");
    SliderIntWithReset("Black", &g_dec->black, 0, 50, "%d", 0);
    SliderIntWithReset("White", &g_dec->white, 50, 100, "%d", 100);
    ImGui::TextUnformatted("Tone");
    SliderFloatWithReset("Contrast", &g_dec->contrast, -100.0f, 100.0f, "%.0f", 0.0f);
    SliderFloatWithReset("Saturation", &g_dec->saturation, 0.0f, 200.0f, "%.0f", 100.0f);
    SliderIntWithReset("Vignetting", &g_dec->vignette, -100, 100, "%d", 0);
    ImGui::TextDisabled("+ lightens corners, - darkens");
    SliderIntWithReset("Shadows", &g_dec->shadows, -100, 100, "%d", 0);
    SliderIntWithReset("Highlights", &g_dec->highlights, -100, 100, "%d", 0);
    SliderFloatWithReset("Clarity", &g_dec->clarity, -100.0f, 100.0f, "%.0f", 0.0f);
    SliderFloatWithReset("Clarity radius", &g_dec->clarityRadius, 0.5f, 3.0f, "%.1f px", 2.0f);

    if (ImGui::CollapsingHeader("Color")) {
        SliderIntWithReset("Temperature", &g_dec->temperature, -100, 100, "%d", 0);
        SliderIntWithReset("Tint", &g_dec->tint, -100, 100, "%d", 0);
        SliderIntWithReset("Vibrance", &g_dec->vibrance, -100, 100, "%d", 0);
    }
    if (ImGui::CollapsingHeader("Detail")) {
        SliderIntWithReset("Sharpening", &g_dec->sharpening, -100, 100, "%d", 0);
        SliderFloatWithReset("Sharpening radius", &g_dec->sharpenRadius, 0.5f, 3.0f, "%.1f px", 1.0f);
        SliderIntWithReset("Noise reduction", &g_dec->denoise, 0, 100, "%d", 0);
    }
    if (ImGui::CollapsingHeader("Lens corrections")) {
        SliderIntWithReset("Chromatic aberration", &g_dec->ca, -100, 100, "%d", 0);
        SliderIntWithReset("Distortion", &g_dec->distortion, -100, 100, "%d", 0);
    }
    if (ImGui::CollapsingHeader("Geometry")) {
        const char* rotationNames[] = { "0 degrees", "90 degrees", "180 degrees", "270 degrees" };
        int rotationIndex = ((g_dec->rotate % 360) + 360) % 360 / 90;
        if (ImGui::Combo("Rotation", &rotationIndex, rotationNames, 4)) {
            g_dec->rotate = rotationIndex * 90;
            MarkToneDirty();
            g_controlChanged = true;
        }
        if (ImGui::Button("Rotate left")) {
            g_dec->rotate = (g_dec->rotate + 270) % 360;
            MarkToneDirty();
            g_controlChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Rotate right")) {
            g_dec->rotate = (g_dec->rotate + 90) % 360;
            MarkToneDirty();
            g_controlChanged = true;
        }
        float crop[4] = { g_dec->cropLeft, g_dec->cropTop, g_dec->cropRight, g_dec->cropBottom };
        if (ImGui::DragFloat4("Crop L/T/R/B", crop, 0.005f, 0.0f, 1.0f, "%.3f")) {
            g_dec->cropLeft = crop[0];
            g_dec->cropTop = crop[1];
            g_dec->cropRight = crop[2];
            g_dec->cropBottom = crop[3];
            MarkToneDirty();
            g_controlChanged = true;
        }
        if (ImGui::SmallButton("Reset geometry")) {
            g_dec->cropLeft = 0.0f;
            g_dec->cropTop = 0.0f;
            g_dec->cropRight = 1.0f;
            g_dec->cropBottom = 1.0f;
            g_dec->rotate = 0;
            MarkToneDirty();
            g_controlChanged = true;
        }
        ImGui::TextDisabled("Crop values are normalized to the source image");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("White balance");
    if (ImGui::BeginCombo("##wbpreset", kWbPresets[g_dec->wbPreset])) {
        for (int i = 0; i < 8; ++i) {
            if (ImGui::Selectable(kWbPresets[i], g_dec->wbPreset == i)) {
                if (i < 7) {
                    g_dec->wb[0] = kWbPresetValues[i][0];
                    g_dec->wb[1] = kWbPresetValues[i][1];
                    g_dec->wb[2] = kWbPresetValues[i][2];
                }
                g_dec->wbPreset = i;
                g_previewDirty = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::DragFloat3("Gains", g_dec->wb, 0.005f, 0.20f, 3.00f, "%.2f")) {
        g_dec->wbPreset = 7;
        g_previewDirty = true;
    }
    ImGui::Checkbox("Pick from preview (click image)", &g_wbPicking);
    if (g_wbPicking && g_dofPicking) g_dofPicking = false;
    if (!g_dec->loaded) ImGui::TextDisabled("Load a RAW first");

    ImGui::Separator();
    ImGui::TextUnformatted("Tone curve");
    if (ImGui::SmallButton("Reset curve")) {
        for (int i = 0; i < 5; ++i) g_dec->curveY[i] = 0.25f * (float)i;
        g_previewDirty = true;
    }
    if (CurveEditor("##tonecurve", g_dec->curveY)) g_previewDirty = true;
    ImGui::Separator();
    if (ImGui::Button("Reset tone")) {
        g_dec->exposure = 0.0f; g_dec->black = 0; g_dec->white = 100;
        g_dec->contrast = 0.0f; g_dec->saturation = 100.0f; g_dec->vignette = 0;
        g_dec->temperature = 0; g_dec->tint = 0; g_dec->vibrance = 0;
        g_dec->clarity = 0.0f; g_dec->clarityRadius = 2.0f;
        g_dec->sharpening = 0; g_dec->sharpenRadius = 1.0f; g_dec->denoise = 0;
        g_dec->ca = 0; g_dec->distortion = 0;
        g_dec->shadows = 0; g_dec->highlights = 0;
        g_dec->wb[0] = g_dec->wb[1] = g_dec->wb[2] = 1.0f; g_dec->wbPreset = 0;
        for (int i = 0; i < 5; ++i) g_dec->curveY[i] = 0.25f * (float)i;
        g_previewDirty = true;
    }
    ImGui::TextDisabled("Live CPU point ops on decoded preview");

    ImGui::Separator();
    ImGui::TextUnformatted("Depth of field");
    SliderIntWithReset("Blur strength", &g_dec->blurAmount, 0, 100, "%d", 0);
    int falloff = (int)std::lround(g_dec->blurRange * 100.0f);
    if (ImGui::SliderInt("Falloff", &falloff, 5, 100, "%d")) {
        g_dec->blurRange = (float)falloff / 100.0f;
        g_previewDirty = true;
    }
    if (ImGui::Checkbox("Pick focus (click image)", &g_dofPicking) && g_dofPicking)
        g_wbPicking = false;
    ImGui::SameLine();
    if (ImGui::SmallButton("Center focus")) {
        g_dec->blurFocusX = 0.5f;
        g_dec->blurFocusY = 0.5f;
        g_previewDirty = true;
    }
    if (g_dec->blurAmount > 0)
        ImGui::TextDisabled("Focus at %.2f, %.2f", g_dec->blurFocusX, g_dec->blurFocusY);
    ImGui::TextDisabled("Distance from the focus point drives the blur");

    ImGui::Separator();
    ImGui::TextUnformatted("Subject mask");
    if (ImGui::Checkbox("Blur outside painted area", &g_dec->maskEnabled)) {
        if (g_dec->maskEnabled && g_dec->blurAmount == 0) g_dec->blurAmount = 50;
        g_previewDirty = true;
    }
    if (g_dec->maskEnabled) {
        if (ImGui::Checkbox("Paint subject (click image)", &g_maskPainting) && g_maskPainting) {
            g_wbPicking = false;
            g_dofPicking = false;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Erase", &g_dec->brushErase);
        ImGui::SliderInt("Brush size", &g_dec->brushSize, 2, 150, "%d");
        if (ImGui::Button("Clear mask")) {
            g_dec->mask.clear();
            MarkMaskDirty(g_dec);
        }
        ImGui::TextDisabled("Paint over the subject; everything else is blurred");
    }

    bool hasImage = g_dec->loaded && g_dec->pw > 0;
    ImGui::Separator();
    ImGui::TextUnformatted("Export");
    if (!hasImage) ImGui::BeginDisabled();
    SliderIntWithReset("JPEG quality", &g_dec->jpegQuality, 50, 100, "%d", 90);
    if (g_dec->hasFullRes) {
        ImGui::Checkbox("Full resolution", &g_dec->exportFullRes);
        if (g_dec->exportFullRes)
            ImGui::TextDisabled("Native %dx%d -> tone-mapped copy",
                                g_dec->fw, g_dec->fh);
        else
            ImGui::TextDisabled("Preview %dx%d -> tone-mapped copy",
                                g_dec->pw, g_dec->ph);
    } else {
        ImGui::TextDisabled("Native %dx%d too large for browser memory; preview export only",
                            g_dec->fw, g_dec->fh);
    }
    if (ImGui::Button("Export PNG")) RequestExport(0);
    ImGui::SameLine();
    if (ImGui::Button("Export JPEG")) RequestExport(1);
    if (g_dec->exportStatus[0]) ImGui::TextWrapped("%s", g_dec->exportStatus);
    if (!hasImage) { ImGui::EndDisabled(); ImGui::TextDisabled("Load a RAW first"); }
    ImGui::End();
}

static void DrawPreview()
{
    ImGui::Begin("Preview", &g_showPreview);
    if (!(g_tex && g_texW > 0 && g_texH > 0)) {
        if (g_dec->id > 0 && !g_dec->loaded)
            ImGui::TextDisabled("Loading %s", g_dec->lastStatus);
        else
            ImGui::TextDisabled(g_dec->loaded ? "Preparing preview..." : "No image loaded");
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
    if (ImGui::Checkbox("Before/After", &g_beforeAfter) && g_beforeAfter)
        BuildBeforePreview(g_dec);
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", g_texW, g_texH);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    g_previewContentSize = avail;
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
    if (g_wbPicking && hovered && ImGui::IsMouseClicked(0) && g_dec->loaded && g_dec->pw > 0) {
        float u = (mouse.x - tlx) / iw, v = (mouse.y - tly) / ih;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            int outputX = std::clamp((int)(u * (float)(g_texW - 1)), 0, g_texW - 1);
            int outputY = std::clamp((int)(v * (float)(g_texH - 1)), 0, g_texH - 1);
            int px, py;
            MapOutputToSource(outputX, outputY, px, py);
            float sum[3] = { 0, 0, 0 };
            int ns = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = px + dx, sy = py + dy;
                    if (sx < 0 || sx >= g_dec->pw || sy < 0 || sy >= g_dec->ph) continue;
                    const float* s = &g_dec->rgb[((size_t)sy * g_dec->pw + sx) * 3];
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
                for (int c = 0; c < 3; ++c) g_dec->wb[c] = gain[c];
                g_dec->wbPreset = 7;
                g_wbPicking = false;
                g_previewDirty = true;
            }
        }
    }

    // Depth-of-field focus picking: the focus is stored in output-normalized
    // coordinates, which is exactly the space the falloff is evaluated in.
    if (g_dofPicking && hovered && ImGui::IsMouseClicked(0) && g_dec->loaded) {
        float u = (mouse.x - tlx) / iw, v = (mouse.y - tly) / ih;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            g_dec->blurFocusX = std::clamp(u, 0.0f, 1.0f);
            g_dec->blurFocusY = std::clamp(v, 0.0f, 1.0f);
            g_previewDirty = true;
        }
    }

    // Subject-mask painting. The mask is stored in output-normalized space, so
    // it lines up with what is on screen and survives a resolution change.
    if (g_maskPainting && g_dec->maskEnabled && g_dec->loaded && hovered) {
        const float u = (mouse.x - tlx) / iw, v = (mouse.y - tly) / ih;
        const bool inside = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
        if (inside && (ImGui::IsMouseClicked(0) || ImGui::IsMouseDown(0))) {
            PaintMask(g_dec, u, v, (float)g_dec->brushSize / (float)std::max(g_texW, g_texH),
                      g_dec->brushErase);
            MarkMaskDirty(g_dec);
        }
        if (inside && ImGui::IsMouseClicked(0)) g_wbPicking = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(20, 20, 24, 255));
    const bool split = g_beforeAfter && g_dec->beforeTexture != 0 &&
                       g_dec->beforeWidth == g_texW && g_dec->beforeHeight == g_texH;
    if (split) {
        const float midX = origin.x + avail.x * 0.5f;
        dl->PushClipRect(origin, ImVec2(midX, origin.y + avail.y), true);
        dl->AddImage((ImTextureID)(intptr_t)g_dec->beforeTexture,
                     ImVec2(tlx, tly), ImVec2(tlx + iw, tly + ih));
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(midX, origin.y), ImVec2(origin.x + avail.x, origin.y + avail.y), true);
        dl->AddImage((ImTextureID)(intptr_t)g_tex, ImVec2(tlx, tly), ImVec2(tlx + iw, tly + ih));
        dl->PopClipRect();
        dl->AddLine(ImVec2(midX, origin.y), ImVec2(midX, origin.y + avail.y), IM_COL32(240, 240, 240, 200), 1.0f);
        dl->AddText(ImVec2(origin.x + 8, origin.y + 6), IM_COL32(255, 255, 255, 220), "Before");
        dl->AddText(ImVec2(midX + 8, origin.y + 6), IM_COL32(255, 255, 255, 220), "After");
    } else {
        dl->AddImage((ImTextureID)(intptr_t)g_tex, ImVec2(tlx, tly), ImVec2(tlx + iw, tly + ih));
    }
    if (g_dec->loaded && g_dec->blurAmount > 0) {
        const float fx = tlx + g_dec->blurFocusX * iw;
        const float fy = tly + g_dec->blurFocusY * ih;
        dl->AddCircle(ImVec2(fx, fy), 7.0f, IM_COL32(255, 255, 255, 210), 0, 1.0f);
        dl->AddCircleFilled(ImVec2(fx, fy), 2.5f, IM_COL32(255, 220, 80, 240));
    }
    if (g_dec->maskEnabled && !g_dec->mask.empty()) {
        if (g_dec->maskTextureRevision != g_dec->maskRevision) {
            std::vector<unsigned char> overlay((size_t)kMaskGrid * kMaskGrid * 4);
            for (size_t i = 0; i < (size_t)kMaskGrid * kMaskGrid; ++i) {
                const unsigned char keep = g_dec->mask[i];
                const bool blurred = keep < 128;
                overlay[i * 4 + 0] = blurred ? (unsigned char)220 : (unsigned char)40;
                overlay[i * 4 + 1] = blurred ? (unsigned char)60 : (unsigned char)210;
                overlay[i * 4 + 2] = blurred ? (unsigned char)60 : (unsigned char)160;
                overlay[i * 4 + 3] = (unsigned char)(blurred ? 70 : 60);
            }
            if (!g_dec->maskTexture) glGenTextures(1, &g_dec->maskTexture);
            glBindTexture(GL_TEXTURE_2D, g_dec->maskTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kMaskGrid, kMaskGrid, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, overlay.data());
            g_dec->maskTextureRevision = g_dec->maskRevision;
        }
        dl->AddImage((ImTextureID)(intptr_t)g_dec->maskTexture,
                     ImVec2(tlx, tly), ImVec2(tlx + iw, tly + ih));
        if (g_maskPainting) {
            const float u = std::clamp((mouse.x - tlx) / iw, 0.0f, 1.0f);
            const float v = std::clamp((mouse.y - tly) / ih, 0.0f, 1.0f);
            const float brush = (float)g_dec->brushSize / (float)std::max(g_texW, g_texH);
            dl->AddCircle(ImVec2(tlx + u * iw, tly + v * ih), std::max(2.0f, brush * iw),
                          IM_COL32(255, 255, 255, 200), 0, 1.5f);
        }
    }

    char zbuf[48];
    snprintf(zbuf, sizeof(zbuf), "%d%%%s", (int)(viewScale * 100.0f),
             g_zoomFit ? " (fit)" : "");
    dl->AddText(ImVec2(origin.x + 6, origin.y + avail.y - 30),
                IM_COL32(255, 255, 255, 220), zbuf);

    ImGui::End();
}

static void DrawStatus()
{
    ImGui::Begin("Status", &g_showStatus);
    ImGui::TextWrapped("%s", g_dec->lastStatus);
    if (g_dec->loaded) {
        ImGui::Separator();
        ImGui::Text("Camera: %s %s", g_dec->make, g_dec->model);
        char cam[128] = "";
        if (g_dec->iso > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "ISO %.0f  ", g_dec->iso);
        if (g_dec->shutter > 0.f) {
            if (g_dec->shutter >= 1.f)
                snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "1/%.0fs  ", g_dec->shutter);
            else
                snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "%.1fs  ", g_dec->shutter);
        }
        if (g_dec->aperture > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "f/%.1f  ", g_dec->aperture);
        if (g_dec->focal > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "%.0fmm", g_dec->focal);
        if (cam[0]) ImGui::Text("%s", cam);
        if (g_dec->lens[0]) ImGui::TextDisabled("Lens: %s", g_dec->lens);
        ImGui::Separator();
        ImGui::Text("Source: %d x %d, %d-bit, %d channels",
                    g_dec->w, g_dec->h, g_dec->bits, g_dec->colors);
        ImGui::Text("Preview: %d x %d (max %d)", g_dec->pw, g_dec->ph, kPreviewMaxDim);
        if (g_dec->renderedWidth > 0 && g_dec->renderedHeight > 0)
            ImGui::Text("Output: %d x %d", g_dec->renderedWidth, g_dec->renderedHeight);
        ImGui::Text("Render quality: %s", g_dec->renderedQuality == 1 ? "final" : "draft");
        ImGui::Text("EV %+.2f  Black %d  White %d  Contrast %.0f  Sat %.0f",
                    g_dec->exposure, g_dec->black, g_dec->white, g_dec->contrast, g_dec->saturation);
    }
    if (g_showFps) ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::End();
}

static void HandleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (io.KeyShift) RedoEdit();
        else UndoEdit();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) RedoEdit();
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) AddSnapshot();

    if (ImGui::IsKeyPressed(ImGuiKey_0)) ResetPreviewView();
    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        g_zoomFit = false;
        g_zoom = 1.0f;
        g_pan = ImVec2(0.0f, 0.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal)) {
        g_zoomFit = false;
        g_zoom = std::min(16.0f, g_zoom * 1.25f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus)) {
        g_zoomFit = false;
        g_zoom = std::max(0.05f, g_zoom / 1.25f);
    }

    if (!g_images.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
        bool forward = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        int current = 0;
        for (int i = 0; i < (int)g_images.size(); ++i)
            if (&g_images[i] == g_dec) current = i;
        int next = (current + (forward ? 1 : -1) + (int)g_images.size()) % (int)g_images.size();
        SelectDocument(&g_images[next]);
    }
}

static void DrawMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (g_menuFont) ImGui::PushFont(g_menuFont);

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Import Image(s)...")) {
#ifdef __EMSCRIPTEN__
            js_open_file_dialog();
#else
            g_showStatus = true;
#endif
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export PNG")) {
            g_showToolbox = true;
            RequestExport(0);
        }
        if (ImGui::MenuItem("Export JPEG")) {
            g_showToolbox = true;
            RequestExport(1);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save profile...")) {
#ifdef __EMSCRIPTEN__
            js_save_profile();
#endif
        }
        if (ImGui::MenuItem("Load profile...")) {
#ifdef __EMSCRIPTEN__
            js_load_profile();
#endif
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close all panels")) {
            g_showPreview = false;
            g_showNavigator = false;
            g_showFilmstrip = false;
            g_showHistory = false;
            g_showToolbox = false;
            g_showHistogram = false;
            g_showStatus = false;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Preview", nullptr, &g_showPreview);
        ImGui::MenuItem("Toolbox", nullptr, &g_showToolbox);
        ImGui::MenuItem("Navigator", nullptr, &g_showNavigator);
        ImGui::MenuItem("Histogram", nullptr, &g_showHistogram);
        ImGui::MenuItem("Filmstrip", nullptr, &g_showFilmstrip);
        ImGui::MenuItem("History", nullptr, &g_showHistory);
        ImGui::MenuItem("Status", nullptr, &g_showStatus);
        ImGui::Separator();
        if (ImGui::MenuItem("Show all panels")) {
            g_showPreview = true;
            g_showNavigator = true;
            g_showFilmstrip = true;
            g_showHistory = true;
            g_showToolbox = true;
            g_showHistogram = true;
            g_showStatus = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Light theme", nullptr, g_lightTheme)) {
            g_themeDirty = true;
            SaveUiSettings();
        }
        ImGui::Separator();

        float uiSize = g_fontSize;
        if (ImGui::SliderFloat("Panel text size", &uiSize, 10.0f, 24.0f, "%.0f px")) {
            if (uiSize != g_fontSize) {
                g_fontSize = uiSize;
                g_fontDirty = true;
                SaveUiSettings();
            }
        }
        float menuSize = g_menuFontSize;
        if (ImGui::SliderFloat("Menu text size", &menuSize, 10.0f, 28.0f, "%.0f px")) {
            if (menuSize != g_menuFontSize) {
                g_menuFontSize = menuSize;
                g_fontDirty = true;
                SaveUiSettings();
            }
        }
        if (ImGui::MenuItem("Load font...")) {
#ifdef __EMSCRIPTEN__
            js_open_font_dialog();
#endif
        }
        if (ImGui::MenuItem("Reset font to default")) {
            g_fontData.clear();
            g_fontName = "Default";
            g_fontSize = 15.0f;
            g_fontDirty = true;
            SaveUiSettings();
#ifdef __EMSCRIPTEN__
            js_forget_font();
#endif
        }
        ImGui::TextDisabled("Panels: %s", g_fontName.c_str());
        ImGui::Separator();

        if (ImGui::MenuItem("Show FPS", nullptr, g_showFps)) SaveUiSettings();
        if (ImGui::MenuItem("Reset panel layout")) {
            // Deferred: tearing down a dock node is not safe mid-frame.
            g_layoutResetPending = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Import images")) g_showToolbox = true;
        ImGui::Separator();
        ImGui::TextDisabled("Ctrl+O  Import images");
        ImGui::TextDisabled("Ctrl+Z / Ctrl+Y  Undo / Redo");
        ImGui::TextDisabled("Drop files anywhere to add them");
        ImGui::EndMenu();
    }

    // PopFont must happen while the window is still open: ImGui verifies the
    // font stack inside End(), so popping after EndMainMenuBar() is an error.
    if (g_menuFont) ImGui::PopFont();
    ImGui::EndMainMenuBar();
}

static void RenderFrame(GLFWwindow* window)
{
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
#ifdef __EMSCRIPTEN__
    ApplyDisplayScale(); // layout in CSS px, framebuffer at devicePixelRatio
#endif
    // The font atlas may only be rebuilt between frames.
    if (g_layoutResetPending) {
        g_layoutResetPending = false;
        ImGui::LoadIniSettingsFromMemory("");
#ifdef __EMSCRIPTEN__
        js_forget_layout();
#endif
    }
    if (g_fontDirty) ApplyFontAtlas();
    if (g_themeDirty) { ApplyTheme(); g_themeDirty = false; }
    ImGui::NewFrame();
    g_controlHovered = false;    HandleShortcuts();

    if (g_previewDirty && g_dec->loaded) RebuildPreview();

    DrawMainMenuBar();

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

    if (g_showPreview) DrawPreview();
    if (g_showNavigator) DrawNavigator();
    if (g_showFilmstrip) DrawFilmstrip();
    if (g_showHistory) DrawHistory();
    if (g_showToolbox) DrawToolbox();
    if (g_showHistogram) DrawHistogram();
    if (g_showStatus) DrawStatus();
    g_controlHovered = ImGui::IsAnyItemHovered() ||
                       ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                                             ImGuiHoveredFlags_DockHierarchy |
                                             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    ImGui::Render();
    int display_w = 0, display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

static void MarkToneDirty()
{
    g_previewDirty = true;
    if (g_dec && g_dec->id > 0) g_dec->revision++;
#ifdef __EMSCRIPTEN__
    if (g_dec && g_dec->loaded) {
        ToneMapPreview();
        g_histDirty = true;
    }
#endif
}

// The mask is pixel data rather than a slider value, so it is deliberately kept
// out of the undo history and the profile: those describe look, not per-image
// geometry of a specific photo.
static void MarkMaskDirty(ImageDocument* doc)
{
    if (!doc) return;
    doc->maskRevision++;
    g_previewDirty = true;
    if (doc->id > 0) doc->revision++;
#ifdef __EMSCRIPTEN__
    if (doc->loaded) {
        ToneMapPreview();
        g_histDirty = true;
    }
#endif
}

static void PaintMask(ImageDocument* doc, float u, float v, float radius, bool erase)
{
    if (!doc) return;
    if (doc->mask.empty()) doc->mask.assign((size_t)kMaskGrid * kMaskGrid, 0);
    const float cx = std::clamp(u, 0.0f, 1.0f) * (float)(kMaskGrid - 1);
    const float cy = std::clamp(v, 0.0f, 1.0f) * (float)(kMaskGrid - 1);
    const float r = std::max(1.0f, radius * (float)(kMaskGrid - 1));
    const int x0 = std::max(0, (int)std::floor(cx - r));
    const int x1 = std::min(kMaskGrid - 1, (int)std::ceil(cx + r));
    const int y0 = std::max(0, (int)std::floor(cy - r));
    const int y1 = std::min(kMaskGrid - 1, (int)std::ceil(cy + r));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > r) continue;
            // Soft edge so the blur transition is not a visible hard line.
            const float falloff = 1.0f - std::clamp(distance / r, 0.0f, 1.0f);
            const float weight = falloff * falloff * (3.0f - 2.0f * falloff);
            const unsigned char target = (unsigned char)(weight * 255.0f);
            unsigned char& cell = doc->mask[(size_t)y * kMaskGrid + x];
            if (erase)
                cell = (unsigned char)std::min((int)cell, 255 - (int)target);
            else
                cell = (unsigned char)std::max((int)cell, (int)target);
        }
    }
}

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
    // The shell restores theme/sizes at runtime init; the first frame picks
    // them up via g_themeDirty rather than flashing the default look.
    ApplyTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 300 es");
#ifdef __EMSCRIPTEN__
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif

    CreatePlaceholderTexture();

    EditState lastEditState = static_cast<const EditState&>(*g_dec);
    bool historyPending = false;
    float historyDelay = 0.0f;
    int lastDocumentId = g_dec->id;

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_BEGIN
#else
    while (!glfwWindowShouldClose(window))
#endif
    {
        if (g_dec->id != lastDocumentId) {
            lastDocumentId = g_dec->id;
            lastEditState = static_cast<const EditState&>(*g_dec);
            historyPending = false;
        }
        bool changedInControls = g_controlChanged;
        g_controlChanged = false;
        if (changedInControls || !EditStateEqual(static_cast<const EditState&>(*g_dec), lastEditState)) {
            if (!changedInControls) MarkToneDirty();
            RequestPreview();
            if (g_skipHistory) {
                g_skipHistory = false;
            } else {
                historyPending = g_dec->id > 0;
                historyDelay = 0.25f;
            }
            lastEditState = static_cast<const EditState&>(*g_dec);
        }
        if (historyPending) {
            historyDelay -= io.DeltaTime;
            if (historyDelay <= 0.0f) {
                RecordHistory(g_dec, "Adjustments");
                historyPending = false;
            }
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
