/* Scalable Vector Extension kernels.
 *
 * Uses only the SVE base instruction set, so this path runs on SVE and SVE2
 * silicon alike (SVE2 implies SVE). It is vector-length agnostic: correct at
 * 128, 256, 512, 1024 and 2048 bits with no recompilation.
 *
 * TWO BUGS THIS FILE EXISTS TO NOT HAVE
 * -------------------------------------
 * The predecessor kernel got both of these wrong. They are worth naming
 * because tools/vane_verify catches each one.
 *
 * 1. INTERLEAVE. Low nibbles are the even elements of the block, high nibbles
 *    the odd ones. Loading the vector with two overlapping contiguous loads
 *    (v+2i and v+2i+1) pairs the wrong operands — those two loads overlap by
 *    all but one lane. svld2_f32 performs the de-interleaving load the format
 *    actually requires: .val[0] = evens, .val[1] = odds.
 *
 * 2. PREDICATION. svmla_f32_z ZEROES inactive lanes. Used on an accumulator
 *    that carries across loop iterations, a partial predicate silently wipes
 *    everything accumulated in the inactive lanes. That is invisible at
 *    128/256/512-bit (where svcntw() divides 16 evenly) and corrupts results
 *    at 1024-bit and above — precisely the length-agnosticism SVE exists to
 *    provide. Accumulators must use the merging form, svmla_f32_m.
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "../vane_internal.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>

extern "C" {

uint32_t vn_sve_vector_bits(void)
{
    return (uint32_t)(svcntb() * 8);
}

void vn_kern_dequant_gemv_int4_sve(const uint8_t *m, const float *scales,
                                   const float *vec, float *out,
                                   int rows, int cols)
{
    const int      nblocks   = cols / VN_BLOCK_SIZE;
    const int      row_bytes = cols / 2;
    const uint32_t vl        = (uint32_t)svcntw();     /* f32 lanes per vector */
    const svbool_t all       = svptrue_b32();

    for (int r = 0; r < rows; ++r) {
        const uint8_t *mrow = m      + (size_t)r * row_bytes;
        const float   *srow = scales + (size_t)r * nblocks;

        svfloat32_t total = svdup_n_f32(0.0f);

        for (int b = 0; b < nblocks; ++b) {
            const uint8_t *bytes = mrow + (size_t)b * VN_BLOCK_BYTES;
            const float   *v     = vec  + (size_t)b * VN_BLOCK_SIZE;

            svfloat32_t acc = svdup_n_f32(0.0f);

            /* 16 bytes -> 32 elements. Iterate over the 16 bytes; each
             * iteration handles vl bytes, producing vl evens and vl odds. */
            for (uint32_t i = 0; i < (uint32_t)VN_BLOCK_BYTES; i += vl) {
                const svbool_t pg = svwhilelt_b32_u32(i, (uint32_t)VN_BLOCK_BYTES);

                /* Load bytes, zero-extending each into a 32-bit lane. */
                const svuint32_t raw = svld1ub_u32(pg, bytes + i);

                /* Split nibbles and remove the +8 zero point. */
                const svint32_t lo = svsub_n_s32_x(pg,
                    svreinterpret_s32_u32(svand_n_u32_x(pg, raw, 0x0Fu)), 8);
                const svint32_t hi = svsub_n_s32_x(pg,
                    svreinterpret_s32_u32(svlsr_n_u32_x(pg, raw, 4)),     8);

                const svfloat32_t lof = svcvt_f32_s32_x(pg, lo);
                const svfloat32_t hif = svcvt_f32_s32_x(pg, hi);

                /* De-interleaving load — see bug note 1. */
                const svfloat32x2_t vv = svld2_f32(pg, v + 2 * i);

                /* Merging predication — see bug note 2. */
                acc = svmla_f32_m(pg, acc, lof, svget2_f32(vv, 0));
                acc = svmla_f32_m(pg, acc, hif, svget2_f32(vv, 1));
            }

            total = svmla_n_f32_x(all, total, acc, srow[b]);
        }
        out[r] = svaddv_f32(all, total);
    }
}

} /* extern "C" */

#elif defined(__aarch64__)
/* Built for an AArch64 target without SVE available to the compiler.
 * The dispatcher will never select this path; these stubs keep the
 * translation unit linkable. */
#include "../vane_internal.h"
extern "C" {
uint32_t vn_sve_vector_bits(void) { return 0; }
void vn_kern_dequant_gemv_int4_sve(const uint8_t *m, const float *scales,
                                   const float *vec, float *out,
                                   int rows, int cols)
{
    vn_kern_dequant_gemv_int4_scalar(m, scales, vec, out, rows, cols);
}
}
#endif
