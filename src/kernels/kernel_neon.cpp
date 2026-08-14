/* ARMv8-A AdvSIMD (NEON) kernels.
 *
 * Compiled at -march=armv8.2-a+simd+dotprod. Never entered unless the
 * dispatcher confirmed the corresponding feature bit at runtime, so this
 * file's baseline does not raise the library's minimum CPU requirement.
 *
 * THE INTERLEAVE, WHICH IS THE WHOLE PROBLEM
 * ------------------------------------------
 * In the INT4 block format, byte j packs element 2j in its low nibble and
 * element 2j+1 in its high nibble. So after masking, the 16 low nibbles are
 * the EVEN elements of the block and the 16 high nibbles are the ODD ones.
 *
 * A naive kernel loads the vector contiguously and multiplies it against
 * those nibbles, which silently pairs the wrong operands. The fix is
 * vld2q_f32, whose de-interleaving load yields .val[0] = evens and
 * .val[1] = odds, matching the nibble layout exactly.
 *
 *   bytes  0..3   low nibbles -> elements  0, 2, 4, 6   == vld2q_f32(v+0).val[0]
 *   bytes  0..3   high nibbles-> elements  1, 3, 5, 7   == vld2q_f32(v+0).val[1]
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "../vane_internal.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#include <math.h>

extern "C" {

void vn_kern_quantize_int4_neon(const float *in, uint8_t *q, float *scales, int n)
{
    const int nblocks = n / VN_BLOCK_SIZE;
    for (int b = 0; b < nblocks; ++b) {
        const float *x = in + (size_t)b * VN_BLOCK_SIZE;

        /* Vectorised max(|x|) over the 32-element block. */
        float32x4_t vmax = vdupq_n_f32(0.0f);
        for (int i = 0; i < VN_BLOCK_SIZE; i += 4)
            vmax = vmaxq_f32(vmax, vabsq_f32(vld1q_f32(x + i)));
        const float maxabs = vmaxvq_f32(vmax);

        const float scale = maxabs / 7.0f;
        scales[b] = scale;
        const float inv = (scale > 1e-20f) ? (1.0f / scale) : 0.0f;

        /* Quantise 32 floats -> 32 nibbles -> 16 bytes.
         * vcvtaq_s32_f32 rounds to nearest, ties away from zero, matching
         * lrintf() in the scalar oracle under the default rounding mode. */
        const float32x4_t vinv = vdupq_n_f32(inv);
        int8_t tmp[VN_BLOCK_SIZE];
        for (int i = 0; i < VN_BLOCK_SIZE; i += 8) {
            int32x4_t a = vcvtaq_s32_f32(vmulq_f32(vld1q_f32(x + i),     vinv));
            int32x4_t c = vcvtaq_s32_f32(vmulq_f32(vld1q_f32(x + i + 4), vinv));
            int16x8_t p = vcombine_s16(vqmovn_s32(a), vqmovn_s32(c));
            p = vminq_s16(vmaxq_s16(p, vdupq_n_s16(-7)), vdupq_n_s16(7));
            vst1_s8(tmp + i, vqmovn_s16(p));
        }

        uint8_t *o = q + (size_t)b * VN_BLOCK_BYTES;
        for (int i = 0; i < VN_BLOCK_BYTES; ++i)
            o[i] = (uint8_t)(((tmp[2 * i] + 8) & 0x0F) |
                             (((tmp[2 * i + 1] + 8) & 0x0F) << 4));
    }
}

void vn_kern_dequant_gemv_int4_neon(const uint8_t *m, const float *scales,
                                    const float *vec, float *out,
                                    int rows, int cols)
{
    const int nblocks   = cols / VN_BLOCK_SIZE;
    const int row_bytes = cols / 2;
    const uint8x16_t nibble_mask = vdupq_n_u8(0x0F);
    const int16x8_t  zero_point  = vdupq_n_s16(8);

    for (int r = 0; r < rows; ++r) {
        const uint8_t *mrow = m      + (size_t)r * row_bytes;
        const float   *srow = scales + (size_t)r * nblocks;

        float32x4_t total = vdupq_n_f32(0.0f);

        for (int b = 0; b < nblocks; ++b) {
            const uint8_t *bytes = mrow + (size_t)b * VN_BLOCK_BYTES;
            const float   *v     = vec  + (size_t)b * VN_BLOCK_SIZE;

            const uint8x16_t raw = vld1q_u8(bytes);
            const uint8x16_t lo8 = vandq_u8(raw, nibble_mask); /* even elements */
            const uint8x16_t hi8 = vshrq_n_u8(raw, 4);         /* odd  elements */

            /* Widen to int16 and remove the +8 zero point. */
            const int16x8_t lo_a = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8 (lo8))), zero_point);
            const int16x8_t lo_b = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(lo8))), zero_point);
            const int16x8_t hi_a = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8 (hi8))), zero_point);
            const int16x8_t hi_b = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(hi8))), zero_point);

            /* De-interleaving loads: .val[0] = evens, .val[1] = odds. */
            const float32x4x2_t v0 = vld2q_f32(v +  0);
            const float32x4x2_t v1 = vld2q_f32(v +  8);
            const float32x4x2_t v2 = vld2q_f32(v + 16);
            const float32x4x2_t v3 = vld2q_f32(v + 24);

            float32x4_t acc = vdupq_n_f32(0.0f);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16 (lo_a))), v0.val[0]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(lo_a))), v1.val[0]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16 (lo_b))), v2.val[0]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(lo_b))), v3.val[0]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16 (hi_a))), v0.val[1]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(hi_a))), v1.val[1]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16 (hi_b))), v2.val[1]);
            acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(hi_b))), v3.val[1]);

            total = vfmaq_n_f32(total, acc, srow[b]);
        }
        out[r] = vaddvq_f32(total);
    }
}

/* INT8 GEMV using FEAT_DotProd. Guarded by __ARM_FEATURE_DOTPROD so the file
 * still compiles for targets without it; the dispatcher checks the runtime
 * bit before ever selecting this path. */
void vn_kern_gemv_int8_dotprod(const int8_t *m, const int8_t *vec, int32_t *out,
                               int rows, int cols)
{
#if defined(__ARM_FEATURE_DOTPROD)
    for (int r = 0; r < rows; ++r) {
        const int8_t *mrow = m + (size_t)r * cols;
        int32x4_t acc = vdupq_n_s32(0);
        int c = 0;
        for (; c + 16 <= cols; c += 16)
            acc = vdotq_s32(acc, vld1q_s8(mrow + c), vld1q_s8(vec + c));
        int32_t s = vaddvq_s32(acc);
        for (; c < cols; ++c)
            s += (int32_t)mrow[c] * (int32_t)vec[c];
        out[r] = s;
    }
#else
    vn_kern_gemv_int8_scalar(m, vec, out, rows, cols);
#endif
}

} /* extern "C" */
#endif /* __aarch64__ */
