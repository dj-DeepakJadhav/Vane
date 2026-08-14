/* Portable C reference implementations — the correctness oracle.
 *
 * These are deliberately simple and portable. They compile everywhere,
 * including on x86 hosts, so the equivalence harness always has something
 * to compare against. Accumulation is float (not double) and grouped
 * per-block so the vector paths differ only by reduction order, not by
 * accumulator width.
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "../vane_internal.h"
#include <math.h>

extern "C" {

void vn_kern_quantize_int4_scalar(const float *in, uint8_t *q, float *scales, int n)
{
    const int nblocks = n / VN_BLOCK_SIZE;
    for (int b = 0; b < nblocks; ++b) {
        const float *x = in + (size_t)b * VN_BLOCK_SIZE;

        float maxabs = 0.0f;
        for (int i = 0; i < VN_BLOCK_SIZE; ++i) {
            const float a = fabsf(x[i]);
            if (a > maxabs) maxabs = a;
        }

        const float scale = maxabs / 7.0f;
        scales[b] = scale;
        const float inv = (scale > 1e-20f) ? (1.0f / scale) : 0.0f;

        uint8_t *o = q + (size_t)b * VN_BLOCK_BYTES;
        for (int i = 0; i < VN_BLOCK_BYTES; ++i) {
            int lo = (int)lrintf(x[2 * i]     * inv);
            int hi = (int)lrintf(x[2 * i + 1] * inv);
            if (lo >  7) lo =  7;
            if (lo < -7) lo = -7;
            if (hi >  7) hi =  7;
            if (hi < -7) hi = -7;
            o[i] = (uint8_t)(((lo + 8) & 0x0F) | (((hi + 8) & 0x0F) << 4));
        }
    }
}

void vn_kern_dequantize_int4_scalar(const uint8_t *q, const float *scales,
                                    float *out, int n)
{
    const int nblocks = n / VN_BLOCK_SIZE;
    for (int b = 0; b < nblocks; ++b) {
        const float scale = scales[b];
        const uint8_t *i8 = q   + (size_t)b * VN_BLOCK_BYTES;
        float         *o  = out + (size_t)b * VN_BLOCK_SIZE;
        for (int i = 0; i < VN_BLOCK_BYTES; ++i) {
            o[2 * i]     = (float)((int)(i8[i] & 0x0F) - 8) * scale;
            o[2 * i + 1] = (float)((int)(i8[i] >> 4)   - 8) * scale;
        }
    }
}

void vn_kern_dequant_gemv_int4_scalar(const uint8_t *m, const float *scales,
                                      const float *vec, float *out,
                                      int rows, int cols)
{
    const int nblocks   = cols / VN_BLOCK_SIZE;
    const int row_bytes = cols / 2;

    for (int r = 0; r < rows; ++r) {
        const uint8_t *mrow = m      + (size_t)r * row_bytes;
        const float   *srow = scales + (size_t)r * nblocks;

        float total = 0.0f;
        for (int b = 0; b < nblocks; ++b) {
            const uint8_t *bytes = mrow + (size_t)b * VN_BLOCK_BYTES;
            const float   *v     = vec  + (size_t)b * VN_BLOCK_SIZE;

            float acc = 0.0f;
            for (int i = 0; i < VN_BLOCK_BYTES; ++i) {
                const int lo = (int)(bytes[i] & 0x0F) - 8;
                const int hi = (int)(bytes[i] >> 4)   - 8;
                acc += (float)lo * v[2 * i];
                acc += (float)hi * v[2 * i + 1];
            }
            total += acc * srow[b];
        }
        out[r] = total;
    }
}

void vn_kern_gemv_int8_scalar(const int8_t *m, const int8_t *vec, int32_t *out,
                              int rows, int cols)
{
    for (int r = 0; r < rows; ++r) {
        const int8_t *mrow = m + (size_t)r * cols;
        int32_t acc = 0;
        for (int c = 0; c < cols; ++c)
            acc += (int32_t)mrow[c] * (int32_t)vec[c];
        out[r] = acc;
    }
}

} /* extern "C" */
