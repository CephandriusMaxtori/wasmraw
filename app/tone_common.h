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
    float cropLeft = 0.f;
    float cropTop = 0.f;
    float cropRight = 1.f;
    float cropBottom = 1.f;
    int rotate = 0;
    float blurAmount = 0.f;
    float blurFocusX = 0.5f;
    float blurFocusY = 0.5f;
    float blurRange = 0.35f;
    // Optional subject-protection mask: a size x size grid in output-normalized
    // space where 255 keeps the pixel sharp and 0 lets the blur through.
    const unsigned char* mask = nullptr;
    int maskSize = 0;
};

static inline void GetCropBounds(int w, int h, const Params& p,
                                 int& left, int& top, int& right, int& bottom)
{
    float x0 = std::clamp(std::min(p.cropLeft, p.cropRight), 0.f, 1.f);
    float x1 = std::clamp(std::max(p.cropLeft, p.cropRight), 0.f, 1.f);
    float y0 = std::clamp(std::min(p.cropTop, p.cropBottom), 0.f, 1.f);
    float y1 = std::clamp(std::max(p.cropTop, p.cropBottom), 0.f, 1.f);
    if (x1 - x0 < 0.001f) x1 = std::min(1.f, x0 + 0.001f);
    if (y1 - y0 < 0.001f) y1 = std::min(1.f, y0 + 0.001f);
    left = std::clamp((int)std::floor(x0 * (float)w), 0, w - 1);
    top = std::clamp((int)std::floor(y0 * (float)h), 0, h - 1);
    right = std::clamp((int)std::ceil(x1 * (float)w), left + 1, w);
    bottom = std::clamp((int)std::ceil(y1 * (float)h), top + 1, h);
}

static inline void GetOutputSize(int w, int h, const Params& p, int& outW, int& outH)
{
    int left, top, right, bottom;
    GetCropBounds(w, h, p, left, top, right, bottom);
    int cropW = std::max(1, right - left);
    int cropH = std::max(1, bottom - top);
    int normalizedRotation = ((p.rotate % 360) + 360) % 360;
    if (normalizedRotation == 90 || normalizedRotation == 270) {
        outW = cropH;
        outH = cropW;
    } else {
        outW = cropW;
        outH = cropH;
    }
}

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

// Depth of field: two box-blurred levels (half and quarter resolution) are
// blended against the sharp image by distance from the focus point. Working on
// a reduced pyramid keeps the cost linear and small enough for live preview.
static inline void BoxBlurRgb(std::vector<float>& buffer, int w, int h, int radius)
{
    if (radius < 1 || w < 1 || h < 1) return;
    const size_t count = (size_t)w * h * 3;
    std::vector<float> temp(count);
    const float inverse = 1.0f / (float)(2 * radius + 1);
    for (int y = 0; y < h; ++y) {
        const size_t row = (size_t)y * w * 3;
        for (int c = 0; c < 3; ++c) {
            float sum = 0.f;
            for (int k = -radius; k <= radius; ++k)
                sum += buffer[row + (size_t)std::clamp(k, 0, w - 1) * 3 + c];
            for (int x = 0; x < w; ++x) {
                temp[row + (size_t)x * 3 + c] = sum * inverse;
                const int add = std::min(x + radius + 1, w - 1);
                const int sub = std::max(x - radius, 0);
                sum += buffer[row + (size_t)add * 3 + c] - buffer[row + (size_t)sub * 3 + c];
            }
        }
    }
    for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
            float sum = 0.f;
            for (int k = -radius; k <= radius; ++k)
                sum += temp[(size_t)std::clamp(k, 0, h - 1) * w * 3 + (size_t)x * 3 + c];
            for (int y = 0; y < h; ++y) {
                buffer[(size_t)y * w * 3 + (size_t)x * 3 + c] = sum * inverse;
                const int add = std::min(y + radius + 1, h - 1);
                const int sub = std::max(y - radius, 0);
                sum += temp[(size_t)add * w * 3 + (size_t)x * 3 + c] -
                       temp[(size_t)sub * w * 3 + (size_t)x * 3 + c];
            }
        }
    }
}

static inline void DownsampleRgb(const std::vector<float>& src, int w, int h,
                                 std::vector<float>& dst, int& dw, int& dh)
{
    dw = std::max(1, w / 2);
    dh = std::max(1, h / 2);
    dst.assign((size_t)dw * dh * 3, 0.f);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            float sum[3] = { 0.f, 0.f, 0.f };
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, w - 1);
                    const int sy = std::min(y * 2 + dy, h - 1);
                    const size_t index = ((size_t)sy * w + sx) * 3;
                    for (int c = 0; c < 3; ++c) sum[c] += src[index + c];
                }
            }
            const size_t index = ((size_t)y * dw + x) * 3;
            for (int c = 0; c < 3; ++c) dst[index + c] = sum[c] * 0.25f;
        }
    }
}

static inline void SampleBilinearRgb(const std::vector<float>& buffer, int w, int h,
                                      float x, float y, float* out)
{
    x = std::clamp(x, 0.f, (float)(w - 1));
    y = std::clamp(y, 0.f, (float)(h - 1));
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - (float)x0, fy = y - (float)y0;
    for (int c = 0; c < 3; ++c) {
        const float a = buffer[((size_t)y0 * w + x0) * 3 + c];
        const float b = buffer[((size_t)y0 * w + x1) * 3 + c];
        const float d = buffer[((size_t)y1 * w + x0) * 3 + c];
        const float e = buffer[((size_t)y1 * w + x1) * 3 + c];
        out[c] = a + (b - a) * fx + (d - a) * fy + (a - b - d + e) * fx * fy;
    }
}

static inline float BlurFalloff(int ox, int oy, int outW, int outH, const Params& p)
{
    const float u = ((float)ox + 0.5f) / (float)outW;
    const float v = ((float)oy + 0.5f) / (float)outH;
    const float dx = (u - std::clamp(p.blurFocusX, 0.f, 1.f)) * (float)outW;
    const float dy = (v - std::clamp(p.blurFocusY, 0.f, 1.f)) * (float)outH;
    const float diagonal = std::sqrt((float)outW * (float)outW + (float)outH * (float)outH);
    float t = std::sqrt(dx * dx + dy * dy) / (std::max(0.02f, p.blurRange) * diagonal);
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

static inline float SampleMask(const unsigned char* mask, int size, float u, float v)
{
    if (!mask || size < 2) return 1.0f;
    const float x = std::clamp(u, 0.f, 1.f) * (float)(size - 1);
    const float y = std::clamp(v, 0.f, 1.f) * (float)(size - 1);
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
    const float fx = x - (float)x0, fy = y - (float)y0;
    const float a = mask[(size_t)y0 * size + x0] / 255.0f;
    const float b = mask[(size_t)y0 * size + x1] / 255.0f;
    const float c = mask[(size_t)y1 * size + x0] / 255.0f;
    const float d = mask[(size_t)y1 * size + x1] / 255.0f;
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static inline void CompositeDepthOfField(const std::vector<float>& work, int outW, int outH,
                                         const Params& p, std::vector<unsigned char>& rgba8)
{
    const float amount = std::clamp(p.blurAmount, 0.f, 1.f);
    std::vector<float> level1, level2;
    int hw, hh, qw, qh;
    DownsampleRgb(work, outW, outH, level1, hw, hh);
    BoxBlurRgb(level1, hw, hh, std::max(1, (int)std::lround(amount * 0.010f * (float)std::max(hw, hh))));
    DownsampleRgb(level1, hw, hh, level2, qw, qh);
    BoxBlurRgb(level2, qw, qh, std::max(1, (int)std::lround(amount * 0.022f * (float)std::max(qw, qh))));

    for (int oy = 0; oy < outH; ++oy) {
        for (int ox = 0; ox < outW; ++ox) {
            const size_t index = ((size_t)oy * outW + ox) * 3;
            const float u = ((float)ox + 0.5f) / (float)outW;
            const float v = ((float)oy + 0.5f) / (float)outH;
            float amount8 = BlurFalloff(ox, oy, outW, outH, p) * amount;
            if (p.mask) {
                // Subject mask wins wherever it protects, so a painted subject
                // stays sharp no matter how far it sits from the focus point.
                const float keep = SampleMask(p.mask, p.maskSize, u, v);
                amount8 = std::max(amount8, (1.0f - keep) * amount);
            }
            float values[3];
            if (amount8 <= 0.f) {
                for (int c = 0; c < 3; ++c) values[c] = work[index + c];
            } else {
                float near[3], far[3];
                const float uHalf = ((float)ox + 0.5f) * 0.5f - 0.5f;
                const float vHalf = ((float)oy + 0.5f) * 0.5f - 0.5f;
                SampleBilinearRgb(level1, hw, hh, uHalf, vHalf, near);
                if (amount8 < 0.5f) {
                    const float mixAmount = amount8 * 2.f;
                    for (int c = 0; c < 3; ++c)
                        values[c] = work[index + c] + (near[c] - work[index + c]) * mixAmount;
                } else {
                    SampleBilinearRgb(level2, qw, qh, uHalf * 0.5f, vHalf * 0.5f, far);
                    const float mixAmount = (amount8 - 0.5f) * 2.f;
                    for (int c = 0; c < 3; ++c)
                        values[c] = near[c] + (far[c] - near[c]) * mixAmount;
                }
            }
            const size_t output = ((size_t)oy * outW + ox) * 4;
            rgba8[output + 0] = (unsigned char)(SrgbEncode(values[0]) * 255.0f + 0.5f);
            rgba8[output + 1] = (unsigned char)(SrgbEncode(values[1]) * 255.0f + 0.5f);
            rgba8[output + 2] = (unsigned char)(SrgbEncode(values[2]) * 255.0f + 0.5f);
            rgba8[output + 3] = 255;
        }
    }
}

static inline void ToneMapToBuffer(const std::vector<float>& src, int w, int h,
                                   const Params& p, std::vector<unsigned char>& rgba8)
{
    int left, top, right, bottom;
    GetCropBounds(w, h, p, left, top, right, bottom);
    int outW, outH;
    GetOutputSize(w, h, p, outW, outH);
    rgba8.resize((size_t)outW * outH * 4);

    const float* source = src.data();
    const size_t sourceCount = (size_t)w * h;
    const float ev = exp2f(p.exposure);
    const float cmul = powf(2.0f, p.contrast);
    const float wb[3] = { p.wbR, p.wbG, p.wbB };
    const float temperatureR = 1.f + p.temperature * 0.20f;
    const float temperatureG = 1.f + p.tint * 0.15f;
    const float temperatureB = 1.f - p.temperature * 0.20f;
    const float outputCx = (float)outW * 0.5f;
    const float outputCy = (float)outH * 0.5f;
    const float rScale = 4.0f / ((float)(outW * outW) + (float)(outH * outH));
    const float maxDim = (float)std::max(w, h);
    const float radiusScale = maxDim / 1600.0f;
    const bool needsLuma = p.clarity != 0.f || p.sharpening != 0.f;
    const float denoiseAmount = p.denoise < 0.f ? 0.f : (p.denoise > 1.f ? 1.f : p.denoise);
    const int normalizedRotation = ((p.rotate % 360) + 360) % 360;
    const int cropW = right - left;
    const int cropH = bottom - top;
    const bool blurActive = p.blurAmount > 0.001f;
    std::vector<float> work;
    if (blurActive) work.resize((size_t)outW * outH * 3);

    std::vector<float> blurY;
    if (needsLuma && maxDim > 0.f) {
        blurY.resize(sourceCount);
        for (size_t i = 0; i < sourceCount; ++i)
            blurY[i] = 0.2126f * source[i * 3] + 0.7152f * source[i * 3 + 1] + 0.0722f * source[i * 3 + 2];

        const float radius = std::max(0.5f, (p.sharpening != 0.f ? p.sharpenRadius : p.clarityRadius) * radiusScale);
        const float sigma = std::max(0.35f, radius * 0.5f);
        const int half = std::max(1, (int)std::ceil(sigma * 2.0f));
        std::vector<float> kernel((size_t)half * 2 + 1);
        float sum = 0.f;
        for (int k = -half; k <= half; ++k) {
            float value = std::exp(-(float)(k * k) / (2.f * sigma * sigma));
            kernel[(size_t)(k + half)] = value;
            sum += value;
        }
        for (float& value : kernel) value /= sum;

        std::vector<float> temporary(sourceCount);
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
        int ix = std::clamp((int)std::floor(x), 0, w - 1);
        int iy = std::clamp((int)std::floor(y), 0, h - 1);
        return source[((size_t)iy * w + ix) * 3 + channel];
    };

    unsigned char* destination = rgba8.data();
    for (int oy = 0; oy < outH; ++oy) {
        for (int ox = 0; ox < outW; ++ox) {
            int ux = ox;
            int uy = oy;
            if (normalizedRotation == 90) {
                ux = oy;
                uy = cropW - 1 - ox;
            } else if (normalizedRotation == 180) {
                ux = cropW - 1 - ox;
                uy = cropH - 1 - oy;
            } else if (normalizedRotation == 270) {
                ux = cropH - 1 - oy;
                uy = ox;
            }
            const int sourceX = left + ux;
            const int sourceY = top + uy;
            const size_t sourceIndex = (size_t)sourceY * w + sourceX;
            const float dx = (float)ox - outputCx;
            const float dy = (float)oy - outputCy;
            const float r2 = (dx * dx + dy * dy) * rScale;
            const float vignetteMultiplier = 1.f + p.vignette * (0.30f * r2 + 0.10f * r2 * r2);
            const float caMultiplier = 1.f + p.ca * 0.18f * r2;

            float sampleX = (float)sourceX;
            float sampleY = (float)sourceY;
            if (p.distortion != 0.f) {
                const float sourceCx = (float)(left + cropW * 0.5);
                const float sourceCy = (float)(top + cropH * 0.5);
                const float sourceDx = sampleX - sourceCx;
                const float sourceDy = sampleY - sourceCy;
                const float sourceR2 = (sourceDx * sourceDx + sourceDy * sourceDy) * rScale;
                const float scale = 1.f + p.distortion * 0.18f * sourceR2;
                sampleX = sourceCx + sourceDx * scale;
                sampleY = sourceCy + sourceDy * scale;
            }

            float values[3];
            for (int channel = 0; channel < 3; ++channel) {
                float value = sample(sampleX, sampleY, channel);
                if (needsLuma)
                    value += (value - blurY[sourceIndex]) * (p.clarity * 0.5f + p.sharpening * 0.7f);
                if (denoiseAmount > 0.f) {
                    float average = 0.f;
                    for (int oySample = -1; oySample <= 1; ++oySample) {
                        for (int oxSample = -1; oxSample <= 1; ++oxSample)
                            average += sample(sampleX + oxSample, sampleY + oySample, channel);
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

            const size_t outputIndex = (size_t)oy * outW + ox;
            if (blurActive) {
                work[outputIndex * 3 + 0] = values[0];
                work[outputIndex * 3 + 1] = values[1];
                work[outputIndex * 3 + 2] = values[2];
            } else {
                destination[outputIndex * 4 + 0] = (unsigned char)(SrgbEncode(values[0]) * 255.0f + 0.5f);
                destination[outputIndex * 4 + 1] = (unsigned char)(SrgbEncode(values[1]) * 255.0f + 0.5f);
                destination[outputIndex * 4 + 2] = (unsigned char)(SrgbEncode(values[2]) * 255.0f + 0.5f);
                destination[outputIndex * 4 + 3] = 255;
            }
        }
    }

    if (blurActive) CompositeDepthOfField(work, outW, outH, p, rgba8);
}

}
