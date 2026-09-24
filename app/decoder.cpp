// Decoder module: runs in a dedicated Web Worker so LibRaw decode and
// full-resolution export never block the UI thread. Compiled as a MODULARIZE
// module (createDecoder) for the browser and driven directly by the smoke test.
#include "libraw.h"
#include "stb_image_write.h"
#include "tone_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <emscripten.h>

static const int kPreviewMaxDim = 1600;

struct Decoded {
    bool loaded = false;
    int w = 0, h = 0, colors = 0, bits = 0;
    int pw = 0, ph = 0;
    std::vector<float> previewRgb;  // pw*ph*3 linear, transferred to the UI
    std::vector<float> fullRgb;     // w*h*3 linear, kept here for export
    char make[64] = {0};
    char model[64] = {0};
    char lens[80] = {0};
    char status[256] = "No image loaded";
    float iso = 0.f, shutter = 0.f, aperture = 0.f, focal = 0.f;
};
static Decoded g_dec;

static std::vector<unsigned char> g_exportBuf;
static std::vector<unsigned char> g_renderedPreview;
static char g_exportStatus[160] = "";

// Decode session (kept alive across phase calls so the browser can show
// progress between the expensive LibRaw stages).
static libraw_data_t* g_lr = nullptr;
static libraw_processed_image_t* g_phaseImg = nullptr;

static void CloseLoadSession()
{
    if (g_phaseImg) { libraw_dcraw_clear_mem(g_phaseImg); g_phaseImg = nullptr; }
    if (g_lr) { libraw_close(g_lr); g_lr = nullptr; }
}

static int PhaseOpen(const unsigned char* buf, int len)
{
    if (!buf || len <= 4) {
        snprintf(g_dec.status, sizeof(g_dec.status), "Empty file");
        return -1000;
    }
    CloseLoadSession();
    g_dec.previewRgb.clear();
    g_dec.fullRgb.clear();
    g_dec.loaded = false;

    g_lr = libraw_init(0);
    if (!g_lr) {
        snprintf(g_dec.status, sizeof(g_dec.status), "libraw_init failed");
        return -1001;
    }
    int err = libraw_open_buffer(g_lr, buf, (size_t)len);
    if (err) {
        snprintf(g_dec.status, sizeof(g_dec.status), "Unsupported or corrupt file (err %d)", err);
        CloseLoadSession();
        return err;
    }
    // Linear 16-bit output so the tone sliders own the mapping.
    g_lr->params.output_bps = 16;
    g_lr->params.no_auto_bright = 1;
    g_lr->params.gamm[0] = 1.0;
    g_lr->params.gamm[1] = 1.0;
    g_lr->params.output_color = 1;   // sRGB primaries
    g_lr->params.use_camera_wb = 1;
    g_lr->params.user_qual = 3;      // AHD demosaic
    snprintf(g_dec.status, sizeof(g_dec.status), "Opened - unpacking pixel data...");
    return 0;
}

static int PhaseUnpack()
{
    if (!g_lr) return -1003;
    int err = libraw_unpack(g_lr);
    if (err) {
        snprintf(g_dec.status, sizeof(g_dec.status), "Unpack failed (err %d)", err);
        CloseLoadSession();
        return err;
    }
    snprintf(g_dec.status, sizeof(g_dec.status), "Unpacked - demosaicing...");
    return 0;
}

static int PhaseProcess()
{
    if (!g_lr) return -1003;
    int err = libraw_dcraw_process(g_lr);
    if (err) {
        snprintf(g_dec.status, sizeof(g_dec.status), "Process failed (err %d)", err);
        CloseLoadSession();
        return err;
    }
    int errc = 0;
    g_phaseImg = libraw_dcraw_make_mem_image(g_lr, &errc);
    if (!g_phaseImg) {
        snprintf(g_dec.status, sizeof(g_dec.status), "Mem image failed (err %d)", errc ? errc : -1002);
        CloseLoadSession();
        return errc ? errc : -1002;
    }
    snprintf(g_dec.status, sizeof(g_dec.status), "Demosaiced - building preview...");
    return 0;
}

// Copies the processed bitmap into a box-downsampled preview (for live UI)
// and a native-resolution copy (for full-res export from this worker).
static void BuildBuffersFromImage(const libraw_processed_image_t* img)
{
    const int w = img->width, h = img->height;
    const int colors = img->colors, bits = img->bits;
    float scale = 1.0f;
    const int maxdim = w > h ? w : h;
    if (maxdim > kPreviewMaxDim) scale = (float)kPreviewMaxDim / (float)maxdim;
    const int pw = std::max(1, (int)(w * scale));
    const int ph = std::max(1, (int)(h * scale));

    std::vector<float> out((size_t)pw * ph * 3, 0.f);
    std::vector<float> cnt((size_t)pw * ph, 0.f);
    std::vector<float> full((size_t)w * h * 3, 0.f);  // native-res copy for export

    const unsigned char* data = img->data;
    const int cpl = colors > 4 ? 4 : colors;

    for (int y = 0; y < h; ++y) {
        int py = (int)(y * scale);
        if (py >= ph) py = ph - 1;
        for (int x = 0; x < w; ++x) {
            int px = (int)(x * scale);
            if (px >= pw) px = pw - 1;
            float r, g, b;
            size_t idx = ((size_t)y * w + x) * cpl;
            float v[4];
            if (bits == 16) {
                const unsigned short* d16 = (const unsigned short*)data;
                for (int ch = 0; ch < cpl; ++ch) v[ch] = d16[idx + ch] / 65535.0f;
            } else {
                for (int ch = 0; ch < cpl; ++ch) v[ch] = data[idx + ch] / 255.0f;
            }
            if (cpl == 1) {
                r = g = b = v[0];
            } else if (cpl == 2) {
                r = v[0]; g = v[1]; b = 0.5f * (v[0] + v[1]);
            } else {
                r = v[0]; g = v[1]; b = v[2];
            }
            full[idx] = r; full[idx + 1] = g; full[idx + 2] = b;
            size_t pi = (size_t)py * pw + px;
            out[pi * 3 + 0] += r;
            out[pi * 3 + 1] += g;
            out[pi * 3 + 2] += b;
            cnt[pi] += 1.0f;
        }
    }
    for (size_t i = 0; i < cnt.size(); ++i) {
        if (cnt[i] > 0.f) {
            out[i * 3 + 0] /= cnt[i];
            out[i * 3 + 1] /= cnt[i];
            out[i * 3 + 2] /= cnt[i];
        }
    }

    g_dec.pw = pw;
    g_dec.ph = ph;
    g_dec.w = w;
    g_dec.h = h;
    g_dec.colors = colors;
    g_dec.bits = bits;
    g_dec.previewRgb = std::move(out);
    g_dec.fullRgb = std::move(full);
}

static int PhaseFinish()
{
    if (g_lr && g_phaseImg && g_phaseImg->type == LIBRAW_IMAGE_BITMAP) {
        libraw_iparams_t* ip = libraw_get_iparams(g_lr);
        if (ip) {
            snprintf(g_dec.make, sizeof(g_dec.make), "%s", ip->make);
            snprintf(g_dec.model, sizeof(g_dec.model), "%s", ip->model);
        }
        g_dec.iso = g_lr->other.iso_speed;
        g_dec.shutter = g_lr->other.shutter;
        g_dec.aperture = g_lr->other.aperture;
        g_dec.focal = g_lr->other.focal_len;
        snprintf(g_dec.lens, sizeof(g_dec.lens), "%s", g_lr->other.desc);
        BuildBuffersFromImage(g_phaseImg);
        CloseLoadSession();
        g_dec.loaded = true;

        char shutterStr[32] = "";
        if (g_dec.shutter > 0.f) {
            if (g_dec.shutter >= 1.f)
                snprintf(shutterStr, sizeof(shutterStr), "1/%.0f", g_dec.shutter);
            else
                snprintf(shutterStr, sizeof(shutterStr), "%.1fs", g_dec.shutter);
        }
        char cam[48] = "";
        if (g_dec.iso > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "ISO %.0f", g_dec.iso);
        if (shutterStr[0])
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "  %s", shutterStr);
        if (g_dec.aperture > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "  f/%.1f", g_dec.aperture);
        if (g_dec.focal > 0.f)
            snprintf(cam + strlen(cam), sizeof(cam) - strlen(cam), "  %.0fmm", g_dec.focal);
        snprintf(g_dec.status, sizeof(g_dec.status),
                 "%s %s - %dx%d (%d-bit, %dch) preview %dx%d%s%s",
                 g_dec.make, g_dec.model, g_dec.w, g_dec.h, g_dec.bits, g_dec.colors,
                 g_dec.pw, g_dec.ph, cam[0] ? " | " : "", cam);
        return 0;
    }
    snprintf(g_dec.status, sizeof(g_dec.status), "Decode failed before preview");
    CloseLoadSession();
    return -1004;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int dec_decode(const unsigned char* buf, int len)
{
    int e = PhaseOpen(buf, len);
    if (!e) e = PhaseUnpack();
    if (!e) e = PhaseProcess();
    if (!e) e = PhaseFinish();
    return e;
}

EMSCRIPTEN_KEEPALIVE
int dec_decode_phase1(const unsigned char* buf, int len) { return PhaseOpen(buf, len); }

EMSCRIPTEN_KEEPALIVE
int dec_decode_phase2() { return PhaseUnpack(); }

EMSCRIPTEN_KEEPALIVE
int dec_decode_phase3() { return PhaseProcess(); }

EMSCRIPTEN_KEEPALIVE
int dec_decode_phase4() { return PhaseFinish(); }

EMSCRIPTEN_KEEPALIVE
const float* dec_preview_ptr() { return g_dec.previewRgb.empty() ? nullptr : g_dec.previewRgb.data(); }

EMSCRIPTEN_KEEPALIVE
int dec_preview_pw() { return g_dec.pw; }

EMSCRIPTEN_KEEPALIVE
int dec_preview_ph() { return g_dec.ph; }

EMSCRIPTEN_KEEPALIVE
int dec_w() { return g_dec.w; }

EMSCRIPTEN_KEEPALIVE
int dec_h() { return g_dec.h; }

EMSCRIPTEN_KEEPALIVE
double dec_iso() { return g_dec.iso; }

EMSCRIPTEN_KEEPALIVE
double dec_shutter() { return g_dec.shutter; }

EMSCRIPTEN_KEEPALIVE
double dec_aperture() { return g_dec.aperture; }

EMSCRIPTEN_KEEPALIVE
double dec_focal() { return g_dec.focal; }

EMSCRIPTEN_KEEPALIVE
const char* dec_meta_make() { return g_dec.make; }

EMSCRIPTEN_KEEPALIVE
const char* dec_meta_model() { return g_dec.model; }

EMSCRIPTEN_KEEPALIVE
const char* dec_meta_lens() { return g_dec.lens; }

EMSCRIPTEN_KEEPALIVE
const char* dec_meta_status() { return g_dec.status; }

EMSCRIPTEN_KEEPALIVE
int dec_render_preview(double exposure, double black, double white, double contrast,
                       double sat, double temperature, double tint,
                       double wbR, double wbG, double wbB, double vignette,
                       double clarity, double clarityRadius, double sharpening,
                       double sharpenRadius, double denoise, double vibrance,
                       double ca, double distortion, double shadows, double highlights,
                       double cy0, double cy1, double cy2, double cy3, double cy4,
                       int quality)
{
    g_renderedPreview.clear();
    if (!g_dec.loaded || g_dec.previewRgb.empty()) return 0;

    tone::Params p;
    p.exposure = (float)exposure;
    p.black = (float)black;
    p.white = (float)white;
    p.contrast = (float)contrast;
    p.saturation = (float)sat;
    p.temperature = (float)temperature;
    p.tint = (float)tint;
    p.wbR = (float)wbR;
    p.wbG = (float)wbG;
    p.wbB = (float)wbB;
    p.vignette = (float)vignette;
    p.clarity = quality == 0 ? 0.f : (float)clarity;
    p.clarityRadius = (float)clarityRadius;
    p.sharpening = quality == 0 ? 0.f : (float)sharpening;
    p.sharpenRadius = (float)sharpenRadius;
    p.denoise = quality == 0 ? 0.f : (float)denoise;
    p.vibrance = (float)vibrance;
    p.ca = (float)ca;
    p.distortion = (float)distortion;
    p.shadows = (float)shadows;
    p.highlights = (float)highlights;
    p.curveY[0] = (float)cy0;
    p.curveY[1] = (float)cy1;
    p.curveY[2] = (float)cy2;
    p.curveY[3] = (float)cy3;
    p.curveY[4] = (float)cy4;
    tone::ToneMapToBuffer(g_dec.previewRgb, g_dec.pw, g_dec.ph, p, g_renderedPreview);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const unsigned char* dec_rendered_preview_ptr()
{
    return g_renderedPreview.empty() ? nullptr : g_renderedPreview.data();
}

EMSCRIPTEN_KEEPALIVE
size_t dec_rendered_preview_size()
{
    return g_renderedPreview.size();
}

EMSCRIPTEN_KEEPALIVE
int dec_rendered_preview_width() { return g_dec.pw; }

EMSCRIPTEN_KEEPALIVE
int dec_rendered_preview_height() { return g_dec.ph; }

// Tone-maps the native-resolution copy with the given params and encodes the
// result (format 0 = PNG, 1 = JPEG). Runs entirely on the worker thread.
EMSCRIPTEN_KEEPALIVE
int dec_export_write(double exposure, double black, double white, double contrast,
                     double sat, double temperature, double tint,
                     double wbR, double wbG, double wbB, double vignette,
                     double clarity, double clarityRadius, double sharpening,
                     double sharpenRadius, double denoise, double vibrance,
                     double ca, double distortion, double shadows, double highlights,
                     double cy0, double cy1, double cy2, double cy3, double cy4,
                     int format, int quality)
{
    g_exportBuf.clear();
    if (!g_dec.loaded || g_dec.fullRgb.empty()) {
        snprintf(g_exportStatus, sizeof(g_exportStatus), "Nothing loaded to export");
        return 0;
    }

    tone::Params p;
    p.exposure = (float)exposure;
    p.black = (float)black;
    p.white = (float)white;
    p.contrast = (float)contrast;
    p.saturation = (float)sat;
    p.temperature = (float)temperature;
    p.tint = (float)tint;
    p.wbR = (float)wbR;
    p.wbG = (float)wbG;
    p.wbB = (float)wbB;
    p.vignette = (float)vignette;
    p.clarity = (float)clarity;
    p.clarityRadius = (float)clarityRadius;
    p.sharpening = (float)sharpening;
    p.sharpenRadius = (float)sharpenRadius;
    p.denoise = (float)denoise;
    p.vibrance = (float)vibrance;
    p.ca = (float)ca;
    p.distortion = (float)distortion;
    p.shadows = (float)shadows;
    p.highlights = (float)highlights;
    p.curveY[0] = (float)cy0;
    p.curveY[1] = (float)cy1;
    p.curveY[2] = (float)cy2;
    p.curveY[3] = (float)cy3;
    p.curveY[4] = (float)cy4;

    std::vector<unsigned char> rgba8;
    tone::ToneMapToBuffer(g_dec.fullRgb, g_dec.w, g_dec.h, p, rgba8);

    struct Ctx { std::vector<unsigned char>* v; };
    auto writer = [](void* context, void* data, int size) {
        auto* v = static_cast<Ctx*>(context)->v;
        const unsigned char* p = static_cast<const unsigned char*>(data);
        v->insert(v->end(), p, p + size);
    };
    Ctx ctx{&g_exportBuf};

    int ok = 0;
    if (format == 0)
        ok = stbi_write_png_to_func(writer, &ctx, g_dec.w, g_dec.h, 4, rgba8.data(), 0);
    else
        ok = stbi_write_jpg_to_func(writer, &ctx, g_dec.w, g_dec.h, 4, rgba8.data(),
                                    quality > 0 ? quality : 90);
    if (ok)
        snprintf(g_exportStatus, sizeof(g_exportStatus), "%s exported: %dx%d, %.1f KB",
                 format == 0 ? "PNG" : "JPEG", g_dec.w, g_dec.h,
                 g_exportBuf.size() / 1024.0);
    else
        snprintf(g_exportStatus, sizeof(g_exportStatus), "Export encode failed");
    return ok;
}

EMSCRIPTEN_KEEPALIVE
size_t dec_export_size() { return g_exportBuf.size(); }

EMSCRIPTEN_KEEPALIVE
const unsigned char* dec_export_data() { return g_exportBuf.empty() ? nullptr : g_exportBuf.data(); }

EMSCRIPTEN_KEEPALIVE
const char* dec_export_status() { return g_exportStatus; }

} // extern "C"