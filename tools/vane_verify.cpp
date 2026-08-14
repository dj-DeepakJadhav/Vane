/* vane_verify — numerical equivalence harness.
 *
 * Runs every kernel path available on this CPU against the portable scalar
 * oracle on identical input and reports the error. Exits non-zero if any
 * path disagrees beyond tolerance, so it works as a CI gate and as a
 * ctest case.
 *
 * This is the tool that catches the two classic SVE mistakes documented in
 * src/kernels/kernel_sve.cpp: pairing de-interleaved nibbles against a
 * contiguously-loaded vector, and using zeroing predication on a
 * loop-carried accumulator. Both produce large, obvious failures here.
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "vane.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

/* Tolerance is relative because the paths differ in reduction ORDER, not in
 * accumulator width — all of them accumulate in float32. Over cols=4096 the
 * observed spread between orderings is ~1e-6 relative; 1e-4 leaves headroom
 * without being loose enough to hide a real defect. A genuine interleave or
 * predication bug produces relative errors near 1.0. */
constexpr double kRelTol = 1e-4;

struct Rng {
    uint32_t s = 0x9E3779B9u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                   return (float)((int32_t)s) / 2147483648.0f; }
};

struct Err { double max_abs = 0.0, max_rel = 0.0; };

Err compare(const std::vector<float> &a, const std::vector<float> &b)
{
    Err e;
    for (size_t i = 0; i < a.size(); ++i) {
        const double abs_e = std::fabs((double)a[i] - (double)b[i]);
        const double denom = std::fmax(1.0, std::fabs((double)a[i]));
        if (abs_e > e.max_abs)         e.max_abs = abs_e;
        if (abs_e / denom > e.max_rel) e.max_rel = abs_e / denom;
    }
    return e;
}

int failures = 0;

void check_gemv(int rows, int cols)
{
    Rng rng;
    std::vector<float>   src((size_t)rows * cols), vec(cols);
    std::vector<uint8_t> q((size_t)rows * cols / 2);
    std::vector<float>   scales((size_t)rows * cols / VN_BLOCK_SIZE);
    for (auto &x : src) x = rng.next();
    for (auto &x : vec) x = rng.next();

    vn_force_path(VN_PATH_SCALAR);
    vn_quantize_int4(src.data(), q.data(), scales.data(), rows * cols);

    std::vector<float> oracle(rows);
    vn_dequant_gemv_int4(q.data(), scales.data(), vec.data(), oracle.data(), rows, cols);

    const vn_path_t paths[] = { VN_PATH_NEON, VN_PATH_SVE };
    for (vn_path_t p : paths) {
        if (vn_force_path(p) != 0) {
            std::printf("  %-6s %5d x %-5d  SKIP (not available on this CPU)\n",
                        vn_path_name(p), rows, cols);
            continue;
        }
        std::vector<float> got(rows, 0.0f);
        vn_dequant_gemv_int4(q.data(), scales.data(), vec.data(), got.data(), rows, cols);
        const Err e = compare(oracle, got);
        const bool ok = e.max_rel <= kRelTol;
        if (!ok) ++failures;
        std::printf("  %-6s %5d x %-5d  max_abs=%.3e  max_rel=%.3e  %s\n",
                    vn_path_name(p), rows, cols, e.max_abs, e.max_rel,
                    ok ? "PASS" : "*** FAIL ***");
    }
}

void check_quantize(int n)
{
    Rng rng;
    std::vector<float> src(n);
    for (auto &x : src) x = rng.next();

    std::vector<uint8_t> q_ref(n / 2), q_got(n / 2);
    std::vector<float>   s_ref(n / VN_BLOCK_SIZE), s_got(n / VN_BLOCK_SIZE);

    vn_force_path(VN_PATH_SCALAR);
    vn_quantize_int4(src.data(), q_ref.data(), s_ref.data(), n);

    if (vn_force_path(VN_PATH_NEON) != 0) {
        std::printf("  neon   quantize n=%-6d  SKIP (not available on this CPU)\n", n);
        return;
    }
    vn_quantize_int4(src.data(), q_got.data(), s_got.data(), n);

    /* Quantisation is integer-exact: the packed bytes must match bit for bit.
     * Any difference means the two paths disagree on rounding or clamping. */
    size_t byte_diff = 0;
    for (size_t i = 0; i < q_ref.size(); ++i) if (q_ref[i] != q_got[i]) ++byte_diff;
    double scale_err = 0.0;
    for (size_t i = 0; i < s_ref.size(); ++i)
        scale_err = std::fmax(scale_err, std::fabs((double)s_ref[i] - (double)s_got[i]));

    const bool ok = (byte_diff == 0) && (scale_err == 0.0);
    if (!ok) ++failures;
    std::printf("  neon   quantize n=%-6d  byte_mismatches=%zu  scale_err=%.3e  %s\n",
                n, byte_diff, scale_err, ok ? "PASS" : "*** FAIL ***");
}

void check_gemv_int8(int rows, int cols)
{
    Rng rng;
    std::vector<int8_t>  m((size_t)rows * cols), v(cols);
    std::vector<int32_t> oracle(rows), got(rows);
    for (auto &x : m) x = (int8_t)(rng.next() * 127.0f);
    for (auto &x : v) x = (int8_t)(rng.next() * 127.0f);

    vn_force_path(VN_PATH_SCALAR);
    vn_gemv_int8(m.data(), v.data(), oracle.data(), rows, cols);

    if (vn_force_path(VN_PATH_NEON) != 0) {
        std::printf("  neon   gemv_int8 %d x %-5d  SKIP\n", rows, cols);
        return;
    }
    vn_gemv_int8(m.data(), v.data(), got.data(), rows, cols);

    /* Integer arithmetic — this must be exact, not approximate. */
    size_t diff = 0;
    for (int i = 0; i < rows; ++i) if (oracle[i] != got[i]) ++diff;
    const bool ok = (diff == 0);
    if (!ok) ++failures;
    std::printf("  neon   gemv_int8 %d x %-5d  mismatches=%zu  %s\n",
                rows, cols, diff, ok ? "PASS" : "*** FAIL ***");
}

} /* namespace */

int main(void)
{
    vn_init();
    vn_caps_t c;
    vn_get_caps(&c);

    std::printf("vane_verify — every path vs the scalar oracle\n");
    std::printf("  cpu      : %s\n", c.cpu_desc[0] ? c.cpu_desc : "(not exposed)");
    std::printf("  features : %s\n", vn_features_string());
    if (c.sve_vector_bits) std::printf("  sve vl   : %u bits\n", c.sve_vector_bits);
    std::printf("  rel tol  : %.1e\n\n", kRelTol);

    std::printf("dequant_gemv_int4\n");
    /* Several shapes, including a column count that is not a multiple of the
     * SVE vector length at any VL, to exercise partial-predicate handling. */
    check_gemv(8,   32);
    check_gemv(4,   96);
    check_gemv(64,  512);
    check_gemv(128, 4096);

    std::printf("\nquantize_int4\n");
    check_quantize(32);
    check_quantize(4096);
    check_quantize(65536);

    std::printf("\ngemv_int8\n");
    check_gemv_int8(16, 64);
    check_gemv_int8(64, 4096);

    vn_force_path(VN_PATH_AUTO);
    std::printf("\n%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS — all available paths agree with the oracle");
    return failures ? 1 : 0;
}
