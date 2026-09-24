#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace tone {

static inline float SrgbEncode(float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
}

struct Params {
    float exposure = 0.f;
    float black = 0.f;
    float white = 1.f;
    float contrast = 0.f;
    float saturation = 1.f;
    float temperature = 0.f;
    float tint = 0.f;
    float wbR = 1.f, wbG = 1.f, wbB = 1.f;
    float vignette = 0.f;
    float clarity = 0.f;
    float clarityRadius = 2.0f;
    float sharpening = 0.f;
    float sharpenRadius = 1.0f;
    float denoise = 0.f;
    float vibrance = 0.f;
    float ca = 0.f;
    float distortion = 0.f;
    float shadows = 0.f;
    float highlights = 0.f;
    float curveY[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };
};

static inline float EvalCurve(const float* y, float x)
{
    const int n = 5;
    const float xs[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };
    if (x <= xs[0] || n == 1) return y[0];
    if (x >= xs[n - 1]) return y[n - 1];
    for (int i = 0; i < n - 1; ++i) {
        if (x <= xs[i + 1]) {
            float t = (x - xs[i]) / (xs[i + 1] - xs[i]);
            t = t * t * (3.f - 2.f * t);
            return y[i] + (y[i + 1] - y[i]) * t;
        }
    }
    return y[n - 1];
}

static inline void ToneMapToBuffer(const std::vector<float>& src, int w, int h,
                                   const Params& p, std::vector<unsigned char>& rgba8)
{
    rgba8.resize((size_t)w * h * 4);
    const float* source = src.data();
    const size_t n = (size_t)w * h;
    const float ev = exp2f(p.exposure);
    const float cmul = powf(2.0f, p.contrast);
    const float wb[3] = { p.wbR, p.wbG, p.wbB };
    const float temperatureR = 1.f + p.temperature * 0.20f;
    const float temperatureG = 1.f + p.tint * 0.15f;
    const float temperatureB = 1.f - p.temperature * 0.20f;
    const float cx = (float)w * 0.5f;
    const float cy = (float)h * 0.5f;
    const float rScale = 4.0f / ((float)(w * w) + (float)(h * h));
    const float maxDim = (float)std::max(w, h);
    const float radiusScale = maxDim / 1600.0f;
    const bool needsLuma = p.clarity != 0.f || p.sharpening != 0.f;
    const float denoiseAmount = p.denoise < 0.f ? 0.f : (p.denoise > 1.f ? 1.f : p.denoise);

    std::vector<float> blurY;
    if (needsLuma && maxDim > 0.f) {
        blurY.resize(n);
        for (size_t i = 0; i < n; ++i)
            blurY[i] = 0.2126f * source[i * 3] + 0.7152f * source[i * 3 + 1] + 0.0722f * source[i * 3 + 2];

        const float radius = std::max(0.5f, (p.sharpening != 0.f ? p.sharpenRadius : p.clarityRadius) * radiusScale);
        const float sigma = std::max(0.35f, radius * 0.5f);
        const int half = std::max(1, (int)ceilf(sigma * 2.0f));
        std::vector<float> kernel((size_t)half * 2 + 1);
        float sum = 0.f;
        for (int k = -half; k <= half; ++k) {
            float value = expf(-(float)(k * k) / (2.f * sigma * sigma));
            kernel[(size_t)(k + half)] = value;
            sum += value;
        }
        for (float& value : kernel) value /= sum;

        std::vector<float> temporary(n);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float value = 0.f;
                for (int k = -half; k <= half; ++k) {
                    int xx = std::clamp(x + k, 0, w - 1);
                    value += blurY[(size_t)y * w + xx] * kernel[(size_t)(k + half)];
                }
                temporary[(size_t)y * w + x] = value;
            }
        }
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                float value = 0.f;
                for (int k = -half; k <= half; ++k) {
                    int yy = std::clamp(y + k, 0, h - 1);
                    value += temporary[(size_t)yy * w + x] * kernel[(size_t)(k + half)];
                }
                blurY[(size_t)y * w + x] = value;
            }
        }
    }

    auto sample = [&](float x, float y, int channel) {
        int ix = std::clamp((int)floorf(x), 0, w - 1);
        int iy = std::clamp((int)floorf(y), 0, h - 1);
        return source[((size_t)iy * w + ix) * 3 + channel];
    };

    unsigned char* destination = rgba8.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t index = (size_t)y * w + x;
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const float r2 = (dx * dx + dy * dy) * rScale;
            const float radial = sqrtf(r2);
            const float vignetteMultiplier = 1.f + p.vignette * (0.30f * r2 + 0.10f * r2 * r2);
            const float caMultiplier = 1.f + p.ca * 0.18f * r2;

            float sampleX = (float)x;
            float sampleY = (float)y;
            if (p.distortion != 0.f) {
                float scale = 1.f + p.distortion * 0.18f * r2;
                sampleX = cx + dx * scale;
                sampleY = cy + dy * scale;
            }

            float values[3];
            for (int channel = 0; channel < 3; ++channel) {
                float value = sample(sampleX, sampleY, channel);
                if (needsLuma)
                    value += (value - blurY[index]) * (p.clarity * 0.5f + p.sharpening * 0.7f);
                if (denoiseAmount > 0.f) {
                    float average = 0.f;
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int ox = -1; ox <= 1; ++ox)
                            average += sample(sampleX + ox, sampleY + oy, channel);
                    }
                    average /= 9.f;
                    value += (average - value) * denoiseAmount * 0.65f;
                }
                float temperature = channel == 0 ? temperatureR : (channel == 1 ? temperatureG : temperatureB);
                float ca = channel == 0 ? caMultiplier : (channel == 2 ? 2.f - caMultiplier : 1.f);
                value *= ev * wb[channel] * temperature * ca * vignetteMultiplier;
                value -= p.black;
                if (p.white > 0.001f) value /= p.white;
                value = 0.5f + (value - 0.5f) * cmul;
                if (p.shadows != 0.f && value < 0.5f) {
                    float weight = value <= 0.f ? 1.f : 1.f - value / 0.5f;
                    weight = weight * weight * (3.f - 2.f * weight);
                    value += p.shadows * weight * 0.35f;
                }
                if (p.highlights != 0.f && value > 0.5f) {
                    float weight = value >= 1.f ? 1.f : (value - 0.5f) / 0.5f;
                    weight = weight * weight * (3.f - 2.f * weight);
                    value -= p.highlights * weight * 0.45f;
                }
                values[channel] = EvalCurve(p.curveY, value);
            }

            float luma = 0.2126f * values[0] + 0.7152f * values[1] + 0.0722f * values[2];
            for (int channel = 0; channel < 3; ++channel) {
                float saturation = p.saturation;
                if (p.vibrance != 0.f) {
                    float maximum = std::max(values[channel], std::max(values[(channel + 1) % 3], values[(channel + 2) % 3]));
                    float minimum = std::min(values[channel], std::min(values[(channel + 1) % 3], values[(channel + 2) % 3]));
                    saturation += p.vibrance * (1.f - (maximum - minimum));
                }
                values[channel] = luma + (values[channel] - luma) * saturation;
            }

            destination[index * 4 + 0] = (unsigned char)(SrgbEncode(values[0]) * 255.0f + 0.5f);
            destination[index * 4 + 1] = (unsigned char)(SrgbEncode(values[1]) * 255.0f + 0.5f);
            destination[index * 4 + 2] = (unsigned char)(SrgbEncode(values[2]) * 255.0f + 0.5f);
            destination[index * 4 + 3] = 255;
        }
    }
}

}
