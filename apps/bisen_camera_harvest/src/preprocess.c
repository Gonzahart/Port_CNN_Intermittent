// preprocess.c -- raw frame to model input.
//
// No sensor access, no globals, no serial. Same input always gives the same
// output, which is why the Python copy of this can be trusted for offline
// debugging.
#include "preprocess.h"
#include "sensor.h"      // PIXEL_SKIPPED
#include "nn_engine.h"   // NN_IN_SCALE, NN_IN_ZP
#include <math.h>

#define GAUSSIAN_BLUR 1     // set to 0 to disable

// Otsu binarization: bright ink -> white(1), dark/paper -> black(0) (MNIST
// polarity for this sensor). No contrast guard, so it never blanks the frame.
#define BINARIZE 1          // set to 0 for plain normalized grayscale

#if GAUSSIAN_BLUR
static void gaussian_blur(const uint16_t in[1024], uint16_t out[1024]) {
    static const int K[3][3] = { {1, 2, 1}, {2, 4, 2}, {1, 2, 1} };
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) {
            if (in[r * 32 + c] == PIXEL_SKIPPED) {
                out[r * 32 + c] = PIXEL_SKIPPED;
                continue;
            }
            uint32_t acc = 0, wsum = 0;
            for (int dr = -1; dr <= 1; dr++) {
                int rr = r + dr;
                if (rr < 0 || rr > 31) continue;
                for (int dc = -1; dc <= 1; dc++) {
                    int cc = c + dc;
                    if (cc < 0 || cc > 31) continue;
                    uint16_t v = in[rr * 32 + cc];
                    if (v == PIXEL_SKIPPED) continue;
                    uint32_t w = (uint32_t)K[dr + 1][dc + 1];
                    acc  += w * v;
                    wsum += w;
                }
            }
            out[r * 32 + c] = (uint16_t)((acc + wsum / 2) / wsum);  // rounded
        }
    }
}
#endif

#if BINARIZE
// Otsu threshold over the non-skipped values, 256 bins of raw>>4.
static int otsu_threshold(const uint16_t *raw) {
    int hist[256];
    for (int b = 0; b < 256; b++) hist[b] = 0;
    int total = 0;
    for (int i = 0; i < 1024; i++)
        if (raw[i] != PIXEL_SKIPPED) { hist[raw[i] >> 4]++; total++; }

    float sum = 0.0f;
    for (int b = 0; b < 256; b++) sum += (float)b * hist[b];
    float sumB = 0.0f; int wB = 0; float maxVar = -1.0f; int thr = 128;
    for (int b = 0; b < 256; b++) {
        wB += hist[b];
        if (wB == 0) continue;
        int wF = total - wB;
        if (wF == 0) break;
        sumB += (float)b * hist[b];
        float mB = sumB / (float)wB;             // mean of dark class
        float mF = (sum - sumB) / (float)wF;     // mean of bright class
        float var = (float)wB * (float)wF * (mB - mF) * (mB - mF);
        if (var > maxVar) { maxVar = var; thr = b; }
    }
    return thr;
}
#endif

void preprocess(const uint16_t raw_in[1024], int8_t in_data[1024]) {
#if GAUSSIAN_BLUR
    uint16_t blur[1024];
    gaussian_blur(raw_in, blur);
    const uint16_t *raw = blur;
#else
    const uint16_t *raw = raw_in;
#endif

    // Input quantization, emitted by the exporter alongside the weights.
    const float scale = NN_IN_SCALE;
    const int   zp    = NN_IN_ZP;

#if BINARIZE
    const int thr = otsu_threshold(raw);
#else
    // min-max over the non-skipped values
    uint16_t lo = 65535, hi = 0;
    for (int i = 0; i < 1024; i++) {
        if (raw[i] != PIXEL_SKIPPED) {
            if (raw[i] < lo) lo = raw[i];
            if (raw[i] > hi) hi = raw[i];
        }
    }
    const float rng = (hi > lo) ? (float)(hi - lo) : 1.0f;
#endif

    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) {
            int src = (31 - r) * 32 + c;       // vertical flip
            float x = 0.0f;                    // masked pixels stay 0
#if BINARIZE
            // bright ink -> white(1); dark/paper + masked -> black(0)
            if (raw[src] != PIXEL_SKIPPED && (raw[src] >> 4) > thr)
                x = 1.0f;
#else
            if (raw[src] != PIXEL_SKIPPED)
                x = ((float)raw[src] - (float)lo) / rng;
#endif
            int q = (int)lroundf(x / scale) + zp;
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            in_data[r * 32 + c] = (int8_t)q;
        }
    }
}
