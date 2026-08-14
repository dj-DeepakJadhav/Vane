/*
 * Vane — runtime ISA dispatch and honest telemetry for Arm
 * Copyright 2026 The Vane Authors. Licensed under the Apache License 2.0.
 *
 * One shared library, one API. At vn_init() the library detects the CPU's
 * vector features and binds each operation to the best available kernel.
 * A binary built here runs correctly on an ARMv8.0 Cortex-A53 and on a
 * Neoverse V2 with 2048-bit SVE2 — the dispatcher guarantees an unsupported
 * instruction path is never entered.
 *
 * INT4 BLOCK FORMAT (canonical throughout this library)
 *   block          = 32 consecutive float32 values
 *   scale          = max(|x|) / 7, one float32 per block
 *   quantised      = clamp(round(x / scale), -7, +7) + 8   -> [1, 15]
 *   packing        = byte j holds q[2j] in the low nibble,
 *                    q[2j+1] in the high nibble; 16 bytes per block
 *   footprint      = 0.5 B/elem packed + 4 B/32 elem scale = 0.625 B/elem
 *                    (68.75% smaller than float16's 2 B/elem)
 *
 * The +8 zero point matches the llama.cpp Q4_0 convention, so weights can be
 * shared with that ecosystem without transcoding.
 */
#ifndef VANE_H
#define VANE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define VN_API __declspec(dllexport)
#else
#  define VN_API __attribute__((visibility("default")))
#endif

#define VN_BLOCK_SIZE  32
#define VN_BLOCK_BYTES 16

/* ---------------------------------------------------------------- features */

typedef enum {
    VN_NEON    = 1u << 0,  /* ARMv8-A AdvSIMD (baseline on all AArch64)      */
    VN_FP16    = 1u << 1,  /* FEAT_FP16 half-precision arithmetic            */
    VN_DOTPROD = 1u << 2,  /* FEAT_DotProd — SDOT/UDOT                       */
    VN_I8MM    = 1u << 3,  /* FEAT_I8MM — SMMLA/USMMLA                       */
    VN_BF16    = 1u << 4,  /* FEAT_BF16                                      */
    VN_SVE     = 1u << 5,  /* Scalable Vector Extension                      */
    VN_SVE2    = 1u << 6,  /* SVE2                                           */
    VN_SME     = 1u << 7,  /* Scalable Matrix Extension                      */
} vn_feature_t;

typedef struct {
    uint32_t features;         /* bitmask of vn_feature_t                     */
    uint32_t sve_vector_bits;  /* runtime SVE vector length, 0 if no SVE      */
    char     cpu_desc[96];     /* best-effort SoC/CPU string; "" if unknown   */
    char     os_desc[32];      /* "android" | "linux" | "macos" | "windows"   */
} vn_caps_t;

/* Must be called once before any kernel. Idempotent and thread-safe.
 * Returns 0 on success, non-zero if no AArch64 SIMD baseline was found. */
VN_API int  vn_init(void);
VN_API void vn_get_caps(vn_caps_t *out);

/* Human-readable feature list, e.g. "neon fp16 dotprod i8mm sve sve2". */
VN_API const char *vn_features_string(void);

/* ---------------------------------------------------------------- dispatch */

typedef enum {
    VN_PATH_AUTO   = 0,  /* best available — the default                     */
    VN_PATH_SCALAR = 1,  /* portable C reference; the correctness oracle     */
    VN_PATH_NEON   = 2,
    VN_PATH_SVE    = 3,
} vn_path_t;

/* Pin the dispatcher to one implementation. Used by the verifier to compare
 * every path against the scalar oracle, and by the benchmark to measure each
 * path on the same silicon. Returns 0 on success, -1 if that path is not
 * available on this CPU (the previous selection is then retained). */
VN_API int         vn_force_path(vn_path_t path);
VN_API vn_path_t   vn_active_path(void);
VN_API const char *vn_path_name(vn_path_t path);

/* ----------------------------------------------------------------- kernels */

/* Quantise n float32 values into the INT4 block format described above.
 *   q      must hold n/2 bytes
 *   scales must hold n/32 floats
 * n must be a multiple of VN_BLOCK_SIZE. */
VN_API void vn_quantize_int4(const float *in, uint8_t *q, float *scales, int n);

/* Inverse of vn_quantize_int4, for error measurement. */
VN_API void vn_dequantize_int4(const uint8_t *q, const float *scales,
                               float *out, int n);

/* Fused dequantise + matrix-vector product — the attention-scoring kernel.
 *
 *   out[r] = sum over c of  dequant(m[r][c]) * vec[c]
 *
 *   m       rows * (cols/2) bytes, INT4 block format, row-major
 *   scales  rows * (cols/32) floats
 *   vec     cols floats
 *   out     rows floats
 * cols must be a multiple of VN_BLOCK_SIZE. */
VN_API void vn_dequant_gemv_int4(const uint8_t *m, const float *scales,
                                 const float *vec, float *out,
                                 int rows, int cols);

/* INT8 matrix-vector product. Uses SDOT where FEAT_DotProd is present.
 *   out[r] = sum over c of m[r][c] * vec[c]
 * cols must be a multiple of 16. */
VN_API void vn_gemv_int8(const int8_t *m, const int8_t *vec, int32_t *out,
                         int rows, int cols);

/* ---------------------------------------------------------------- telemetry */

/* Every field is measured. A field that could not be observed on this
 * platform is emitted as a sentinel below and serialised as JSON null —
 * this library never reports a number it did not take from a live timer. */
#define VN_UNMEASURED (-1.0)

typedef struct {
    char   op[32];
    char   path[16];          /* which kernel actually executed              */
    int    rows, cols;
    int64_t iterations;       /* completed timed iterations                  */
    double median_ns;         /* per-call, from a live monotonic clock       */
    double p95_ns;
    double min_ns;
    double gb_per_sec;        /* bytes touched / elapsed                     */
    double thermal_c_start;   /* VN_UNMEASURED where unavailable             */
    double thermal_c_end;
    double sustained_ratio;   /* late-window throughput / early-window        */
} vn_bench_result_t;

/* Run `op` for `seconds` of wall time and report what was observed.
 * Valid ops: "dequant_gemv_int4", "gemv_int8", "quantize_int4". */
VN_API vn_bench_result_t vn_bench(const char *op, int rows, int cols,
                                  double seconds);

/* Read CPU thermal state in Celsius, or VN_UNMEASURED if this platform
 * exposes no readable sensor. Never estimated. */
VN_API double vn_read_thermal_c(void);

/* Serialise results to JSON. Unmeasured fields are written as null. */
VN_API int vn_write_report(const char *path, const vn_bench_result_t *results,
                           int count);

#ifdef __cplusplus
}
#endif
#endif /* VANE_H */
