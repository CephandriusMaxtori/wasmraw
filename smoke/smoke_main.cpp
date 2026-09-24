#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int dec_decode(const unsigned char* buf, int len);
extern "C" const float* dec_preview_ptr();
extern "C" int dec_preview_pw();
extern "C" int dec_preview_ph();
extern "C" int dec_w();
extern "C" int dec_h();
extern "C" const char* dec_meta_status();
extern "C" int dec_export_write(double exposure, double black, double white,
                                double contrast, double sat,
                                double wbR, double wbG, double wbB, double vignette,
                                double clarity, double clarityRadius,
                                double shadows, double highlights,
                                double cy0, double cy1, double cy2, double cy3, double cy4,
                                int format, int quality);
extern "C" size_t dec_export_size();
extern "C" const unsigned char* dec_export_data();

static const char* kPngMagic = "\x89PNG\r\n\x1a\n";

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: smoke <file>\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(n);
    if (!buf) { fclose(f); return 2; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return 2; }

    int r = dec_decode(buf, (int)n);
    free(buf);
    printf("RESULT=%d\n%s\n", r, dec_meta_status());
    if (r == 0) {
        int pw = dec_preview_pw(), ph = dec_preview_ph();
        const float* pv = dec_preview_ptr();
        size_t count = (size_t)pw * ph * 3;
        if (!pv || count == 0) {
            fprintf(stderr, "no preview data\n");
            return 1;
        }
        float mn = pv[0], mx = pv[0], sum = 0.f;
        for (size_t i = 0; i < count; ++i) {
            if (pv[i] < mn) mn = pv[i];
            if (pv[i] > mx) mx = pv[i];
            sum += pv[i];
        }
        printf("PREVIEW pw=%d ph=%d count=%zu min=%.4f max=%.4f mean=%.4f\n",
               pw, ph, count, (double)mn, (double)mx, (double)(sum / (float)count));

        int okPng = dec_export_write(0.0, 0.0, 1.0, 0.0, 1.0,
                                     1.0, 1.0, 1.0, 0.25,
                                     0.3, 2.0, 0.2, 0.1,
                                     0.0, 0.25, 0.5, 0.75, 1.0, 0, 0);
        size_t pngSz = dec_export_size();
        const unsigned char* png = dec_export_data();
        bool pngOk = okPng && pngSz > 0 && png && memcmp(png, kPngMagic, 8) == 0;
        printf("EXPORT-PNG ok=%d size=%u magic=%d\n", okPng, (unsigned)pngSz,
               pngOk ? 1 : 0);

        int okJpg = dec_export_write(0.0, 0.0, 1.0, 0.0, 1.0,
                                     1.0, 1.0, 1.0, -0.25,
                                     0.0, 2.0, -0.2, -0.1,
                                     0.0, 0.25, 0.5, 0.75, 1.0, 1, 90);
        size_t jpgSz = dec_export_size();
        const unsigned char* jpg = dec_export_data();
        bool jpgOk = okJpg && jpgSz > 0 && jpg && jpg[0] == 0xFF && jpg[1] == 0xD8;
        printf("EXPORT-JPG ok=%d size=%u magic=%d\n", okJpg, (unsigned)jpgSz,
               jpgOk ? 1 : 0);

        if (!pngOk || !jpgOk) r = 1;
    }
    return r ? 1 : 0;
}