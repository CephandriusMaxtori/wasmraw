#pragma once
#include <cmath>
#include <vector>

// Shared tone-mapping helpers used by both the UI module (live preview) and
// the decoder worker (full-resolution export) so the math cannot drift.

namespace tone {

static inline float SrgbEncode(float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
}

struct Params {
    float exposure = 0.f;    // EV
    float black = 0.f;       // fraction 0..0.5
    float white = 1.f;       // fraction 0.5..1
    float contrast = 0.f;    // -1..1
    float saturation = 1.f;  // 0..2
    float wbR = 1.f, wbG = 1.f, wbB = 1.f;  // white-balance channel gains
    float vignette = 0.f;   // -1..1; >0 lightens corners (lens correction), <0 darkens
    float clarity = 0.f;    // -1..1 unsharp strength (luma-based local contrast)
    float clarityRadius = 2.0f;        // blur radius, px
    float shadows = 0.f;    // -1..1; >0 lifts dark areas
    float highlights = 0.f; // -1..1; >0 compresses bright areas
    float curveY[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };  // tone curve (x fixed 0..1)
};

// X positions are fixed to {0,.25,.5,.75,1}; smoothstep between control points.
static inline float EvalCurve(const float* y, float x)
{
    const int n = 5;
    const float xs[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };
    if (x <= xs[0] || n == 1) return y[0];
    if (x >= xs[n - 1]) return y[n - 1];
    for (int i = 0; i < n - 1; ++i) {
        if (x <= xs[i + 1]) {
            float t = (x - xs[i]) / (xs[i + 1] - xs[i]);
            t = t * t * (3.f - 2.f * t); // smoothstep
            return y[i] + (y[i + 1] - y[i]) * t;
        }
    }
    return y[n - 1];
}

// src is w*h*3 linear floats in [0..1]; fills rgba8 (w*h*4, alpha 255).
static inline void ToneMapToBuffer(const std::vector<float>& src, int w, int h,
                                   const Params& p, std::vector<unsigned char>& rgba8)
{
    rgba8.resize((size_t)w * h * 4);
    const float* s = src.data();
    const size_t n = (size_t)w * h;
    const float ev = exp2f(p.exposure);
    const float cmul = powf(2.0f, p.contrast);
    const float wb[3] = { p.wbR, p.wbG, p.wbB };
    // Vignetting polynomial in normalized radius: m(r)=1+vig*(0.30 r^2 + 0.10 r^4).
    const float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    const float rScale = 4.0f / ((float)(w * w) + (float)(h * h));
    const float vig = p.vignette;

    // Clarity: unsharp mask on luminance (pre-tonemap, linear domain).
    std::vector<float> blurY;
    const float clarity = (p.clarity != 0.f && p.clarityRadius > 0.1f) ? p.clarity : 0.f;
    if (clarity != 0.f) {
        blurY.resize(n);
        for (size_t i = 0; i < n; ++i)
            blurY[i] = 0.2126f * s[i * 3] + 0.7152f * s[i * 3 + 1] + 0.0722f * s[i * 3 + 2];

        const float sigma = p.clarityRadius * 0.5f;
        const int half = (int)ceilf(sigma * 2.0f);
        std::vector<float> kern((size_t)half * 2 + 1);
        float ksum = 0.f;
        for (int k = -half; k <= half; ++k) {
            float v = expf(-(float)(k * k) / (2.f * sigma * sigma));
            kern[(size_t)(k + half)] = v;
            ksum += v;
        }
        for (float& k : kern) k /= ksum;

        std::vector<float> tmp(n);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.f;
                for (int k = -half; k <= half; ++k) {
                    int xx = x + k;
                    xx = xx < 0 ? 0 : (xx > w - 1 ? w - 1 : xx);
                    acc += blurY[(size_t)y * w + xx] * kern[(size_t)(k + half)];
                }
                tmp[(size_t)y * w + x] = acc;
            }
        }
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                float acc = 0.f;
                for (int k = -half; k <= half; ++k) {
                    int yy = y + k;
                    yy = yy < 0 ? 0 : (yy > h - 1 ? h - 1 : yy);
                    acc += tmp[(size_t)yy * w + x] * kern[(size_t)(k + half)];
                }
                blurY[(size_t)y * w + x] = acc;
            }
        }
    }

    unsigned char* dst = rgba8.data();
    for (size_t i = 0; i < n; ++i) {
        int ix = (int)(i % (size_t)w), iy = (int)(i / (size_t)w);
        float dx = (float)ix - cx, dy = (float)iy - cy;
        float r2 = (dx * dx + dy * dy) * rScale;
        float m = 1.f + vig * (0.30f * r2 + 0.10f * r2 * r2);
        float v[3];
        for (int ch = 0; ch < 3; ++ch) {
            float fs = s[i * 3 + ch];
            if (clarity != 0.f) fs += (fs - blurY[i]) * clarity * 0.5f;
            float f = fs * ev * wb[ch] * m - p.black;
            f = (p.white > 0.001f) ? f / p.white : f;
            f = 0.5f + (f - 0.5f) * cmul;
            // Shadow / highlight recovery (on the clamped pre-curve value).
            if (p.shadows != 0.f && f < 0.5f) {
                float sw = (f <= 0.f) ? 1.f : 1.f - f / 0.5f;
                sw = sw * sw * (3.f - 2.f * sw);
                f += p.shadows * sw * 0.35f;
            }
            if (p.highlights != 0.f && f > 0.5f) {
                float hw = (f >= 1.f) ? 1.f : (f - 0.5f) / 0.5f;
                hw = hw * hw * (3.f - 2.f * hw);
                f -= p.highlights * hw * 0.45f;
            }
            f = EvalCurve(p.curveY, f);
            v[ch] = f;
        }
        float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        for (int ch = 0; ch < 3; ++ch)
            v[ch] = y + (v[ch] - y) * p.saturation;

        dst[i * 4 + 0] = (unsigned char)(SrgbEncode(v[0]) * 255.0f + 0.5f);
        dst[i * 4 + 1] = (unsigned char)(SrgbEncode(v[1]) * 255.0f + 0.5f);
        dst[i * 4 + 2] = (unsigned char)(SrgbEncode(v[2]) * 255.0f + 0.5f);
        dst[i * 4 + 3] = 255;
    }
}

} // namespace tone