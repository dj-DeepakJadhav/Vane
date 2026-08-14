/* Internal kernel declarations. Not installed. */
#ifndef VANE_INTERNAL_H
#define VANE_INTERNAL_H

#include "vane.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every kernel below implements exactly the contract documented in vane.h.
 * kernel_scalar.cpp is the correctness oracle: tools/vane_verify compares
 * every other implementation against it on identical input. */

void vn_kern_quantize_int4_scalar(const float *in, uint8_t *q, float *scales, int n);
void vn_kern_dequantize_int4_scalar(const uint8_t *q, const float *scales, float *out, int n);
void vn_kern_dequant_gemv_int4_scalar(const uint8_t *m, const float *scales,
                                      const float *vec, float *out, int rows, int cols);
void vn_kern_gemv_int8_scalar(const int8_t *m, const int8_t *vec, int32_t *out,
                              int rows, int cols);

#if defined(__aarch64__)
void vn_kern_quantize_int4_neon(const float *in, uint8_t *q, float *scales, int n);
void vn_kern_dequant_gemv_int4_neon(const uint8_t *m, const float *scales,
                                    const float *vec, float *out, int rows, int cols);
void vn_kern_gemv_int8_dotprod(const int8_t *m, const int8_t *vec, int32_t *out,
                               int rows, int cols);

void vn_kern_dequant_gemv_int4_sve(const uint8_t *m, const float *scales,
                                   const float *vec, float *out, int rows, int cols);
/* Runtime SVE vector length in bits. Only call when VN_SVE is present. */
uint32_t vn_sve_vector_bits(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
