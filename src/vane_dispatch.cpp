/* Runtime ISA detection and kernel binding.
 *
 * The design point: compile each kernel translation unit at its own -march
 * baseline, then choose between them at run time from CPU feature bits. One
 * shared object therefore runs correctly on an ARMv8.0 Cortex-A53 and on a
 * Neoverse V2 with 2048-bit SVE2, using the best instructions each offers,
 * because an unsupported path is never entered.
 *
 * Binding happens once in vn_init(). After that a kernel call is an indirect
 * call through a function pointer with no feature testing on the hot path.
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "vane_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__linux__) || defined(__ANDROID__)
#  include <sys/auxv.h>
#  include <unistd.h>
   /* Defined locally rather than pulled from asm/hwcap.h, whose availability
    * varies across NDK and glibc versions. Values are the AArch64 ABI's. */
#  ifndef VN_HWCAP_ASIMD
#    define VN_HWCAP_ASIMD     (1UL << 1)
#    define VN_HWCAP_ASIMDHP   (1UL << 10)
#    define VN_HWCAP_ASIMDDP   (1UL << 20)
#    define VN_HWCAP_SVE       (1UL << 22)
#    define VN_HWCAP2_SVE2     (1UL << 1)
#    define VN_HWCAP2_I8MM     (1UL << 13)
#    define VN_HWCAP2_BF16     (1UL << 14)
#    define VN_HWCAP2_SME      (1UL << 23)
#  endif
#endif

#if defined(__APPLE__)
#  include <sys/sysctl.h>
#endif

#if defined(_WIN32)
#  include <windows.h>
#endif

/* ------------------------------------------------------------------ state */

static int        g_initialised = 0;
static vn_caps_t  g_caps;
static vn_path_t  g_path = VN_PATH_AUTO;
static char       g_features_str[128];

static void (*p_quantize_int4)(const float *, uint8_t *, float *, int);
static void (*p_dequant_gemv_int4)(const uint8_t *, const float *, const float *,
                                   float *, int, int);
static void (*p_gemv_int8)(const int8_t *, const int8_t *, int32_t *, int, int);

/* -------------------------------------------------------------- detection */

#if defined(__linux__) || defined(__ANDROID__)
static void read_cpu_desc(char *dst, size_t cap)
{
    dst[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        /* Android exposes "Hardware"; server parts expose "CPU part". */
        if (!strncmp(line, "Hardware", 8) || !strncmp(line, "model name", 10)) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *s = colon + 1;
                while (*s == ' ' || *s == '\t') ++s;
                size_t n = strlen(s);
                while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
                snprintf(dst, cap, "%s", s);
                break;
            }
        }
    }
    fclose(f);
}
#endif

static uint32_t detect_features(void)
{
    uint32_t f = 0;

#if defined(__linux__) || defined(__ANDROID__)
    const unsigned long h1 = getauxval(AT_HWCAP);
    const unsigned long h2 = getauxval(AT_HWCAP2);
    if (h1 & VN_HWCAP_ASIMD)   f |= VN_NEON;
    if (h1 & VN_HWCAP_ASIMDHP) f |= VN_FP16;
    if (h1 & VN_HWCAP_ASIMDDP) f |= VN_DOTPROD;
    if (h1 & VN_HWCAP_SVE)     f |= VN_SVE;
    if (h2 & VN_HWCAP2_SVE2)   f |= VN_SVE2;
    if (h2 & VN_HWCAP2_I8MM)   f |= VN_I8MM;
    if (h2 & VN_HWCAP2_BF16)   f |= VN_BF16;
    if (h2 & VN_HWCAP2_SME)    f |= VN_SME;
    snprintf(g_caps.os_desc, sizeof g_caps.os_desc, "%s",
#  if defined(__ANDROID__)
             "android"
#  else
             "linux"
#  endif
    );
    read_cpu_desc(g_caps.cpu_desc, sizeof g_caps.cpu_desc);

#elif defined(__APPLE__)
    f |= VN_NEON;                      /* mandatory on all Apple arm64 */
    int v = 0; size_t sz = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_FP16",    &v, &sz, NULL, 0) == 0 && v) f |= VN_FP16;
    v = 0; sz = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_DotProd", &v, &sz, NULL, 0) == 0 && v) f |= VN_DOTPROD;
    v = 0; sz = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_I8MM",    &v, &sz, NULL, 0) == 0 && v) f |= VN_I8MM;
    v = 0; sz = sizeof v;
    if (sysctlbyname("hw.optional.arm.FEAT_BF16",    &v, &sz, NULL, 0) == 0 && v) f |= VN_BF16;
    /* Apple Silicon implements no SVE as of this writing; the bits above are
     * queried rather than assumed, so this stays correct if that changes. */
    sz = sizeof g_caps.cpu_desc;
    if (sysctlbyname("machdep.cpu.brand_string", g_caps.cpu_desc, &sz, NULL, 0) != 0)
        g_caps.cpu_desc[0] = '\0';
    snprintf(g_caps.os_desc, sizeof g_caps.os_desc, "macos");

#elif defined(_WIN32) && defined(_M_ARM64)
    f |= VN_NEON;
    if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) f |= VN_DOTPROD;
    snprintf(g_caps.os_desc, sizeof g_caps.os_desc, "windows");

#else
    /* Non-AArch64 host (e.g. an x86 developer machine). No vector path is
     * claimed; the scalar oracle still runs so the verifier is usable. */
    snprintf(g_caps.os_desc, sizeof g_caps.os_desc, "non-arm");
#endif

    return f;
}

static void build_features_string(void)
{
    g_features_str[0] = '\0';
    const struct { uint32_t bit; const char *name; } tbl[] = {
        { VN_NEON, "neon" }, { VN_FP16, "fp16" }, { VN_DOTPROD, "dotprod" },
        { VN_I8MM, "i8mm" }, { VN_BF16, "bf16" }, { VN_SVE, "sve" },
        { VN_SVE2, "sve2" }, { VN_SME, "sme" },
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; ++i) {
        if (!(g_caps.features & tbl[i].bit)) continue;
        if (g_features_str[0]) strncat(g_features_str, " ",
                                       sizeof g_features_str - strlen(g_features_str) - 1);
        strncat(g_features_str, tbl[i].name,
                sizeof g_features_str - strlen(g_features_str) - 1);
    }
    if (!g_features_str[0]) snprintf(g_features_str, sizeof g_features_str, "none");
}

/* --------------------------------------------------------------- binding */

static void bind_scalar(void)
{
    p_quantize_int4     = vn_kern_quantize_int4_scalar;
    p_dequant_gemv_int4 = vn_kern_dequant_gemv_int4_scalar;
    p_gemv_int8         = vn_kern_gemv_int8_scalar;
}

static int bind_path(vn_path_t path)
{
    switch (path) {
    case VN_PATH_SCALAR:
        bind_scalar();
        g_path = VN_PATH_SCALAR;
        return 0;

#if defined(__aarch64__)
    case VN_PATH_NEON:
        if (!(g_caps.features & VN_NEON)) return -1;
        p_quantize_int4     = vn_kern_quantize_int4_neon;
        p_dequant_gemv_int4 = vn_kern_dequant_gemv_int4_neon;
        p_gemv_int8         = (g_caps.features & VN_DOTPROD)
                                ? vn_kern_gemv_int8_dotprod
                                : vn_kern_gemv_int8_scalar;
        g_path = VN_PATH_NEON;
        return 0;

    case VN_PATH_SVE:
        if (!(g_caps.features & VN_SVE)) return -1;
        /* Only the GEMV has an SVE implementation today; the remaining
         * operations bind to their best non-SVE version rather than
         * pretending a path exists. */
        p_quantize_int4     = (g_caps.features & VN_NEON)
                                ? vn_kern_quantize_int4_neon
                                : vn_kern_quantize_int4_scalar;
        p_dequant_gemv_int4 = vn_kern_dequant_gemv_int4_sve;
        p_gemv_int8         = (g_caps.features & VN_DOTPROD)
                                ? vn_kern_gemv_int8_dotprod
                                : vn_kern_gemv_int8_scalar;
        g_path = VN_PATH_SVE;
        return 0;
#endif

    case VN_PATH_AUTO:
    default:
#if defined(__aarch64__)
        if ((g_caps.features & VN_SVE)  && bind_path(VN_PATH_SVE)  == 0) return 0;
        if ((g_caps.features & VN_NEON) && bind_path(VN_PATH_NEON) == 0) return 0;
#endif
        bind_scalar();
        g_path = VN_PATH_SCALAR;
        return 0;
    }
}

/* ------------------------------------------------------------------- API */

int vn_init(void)
{
    if (g_initialised) return 0;

    memset(&g_caps, 0, sizeof g_caps);
    g_caps.features = detect_features();

#if defined(__aarch64__)
    /* Do NOT gate this on __ARM_FEATURE_SVE. That macro reflects the flags of
     * THIS translation unit, which is compiled at the library's baseline
     * without +sve — so the guard was always false and the vector length was
     * never read, even on SVE hardware. vn_sve_vector_bits() lives in
     * kernel_sve.cpp, which does get +sve, and the runtime feature bit below
     * is what makes the call safe.
     *
     * Found by running on Graviton4: the probe reported "sve vector len: n/a"
     * while simultaneously reporting sve and sve2 as present and dispatching
     * to the SVE path. */
    if (g_caps.features & VN_SVE) g_caps.sve_vector_bits = vn_sve_vector_bits();
#endif

    build_features_string();
    bind_path(VN_PATH_AUTO);
    g_initialised = 1;
    return 0;
}

void vn_get_caps(vn_caps_t *out)
{
    if (!out) return;
    vn_init();
    *out = g_caps;
}

const char *vn_features_string(void)
{
    vn_init();
    return g_features_str;
}

int vn_force_path(vn_path_t path)
{
    vn_init();
    const vn_path_t previous = g_path;
    if (bind_path(path) != 0) {
        bind_path(previous);
        return -1;
    }
    return 0;
}

vn_path_t vn_active_path(void) { vn_init(); return g_path; }

const char *vn_path_name(vn_path_t path)
{
    switch (path) {
    case VN_PATH_SCALAR: return "scalar";
    case VN_PATH_NEON:   return "neon";
    case VN_PATH_SVE:    return "sve";
    case VN_PATH_AUTO:   return "auto";
    default:             return "unknown";
    }
}

/* Public kernels. Each is a thin forward through the bound pointer, so the
 * call graph from these entry points reaches every kernel in the library —
 * there is no unreachable implementation here. */

void vn_quantize_int4(const float *in, uint8_t *q, float *scales, int n)
{
    vn_init();
    p_quantize_int4(in, q, scales, n);
}

void vn_dequantize_int4(const uint8_t *q, const float *scales, float *out, int n)
{
    vn_init();
    vn_kern_dequantize_int4_scalar(q, scales, out, n);
}

void vn_dequant_gemv_int4(const uint8_t *m, const float *scales,
                          const float *vec, float *out, int rows, int cols)
{
    vn_init();
    p_dequant_gemv_int4(m, scales, vec, out, rows, cols);
}

void vn_gemv_int8(const int8_t *m, const int8_t *vec, int32_t *out,
                  int rows, int cols)
{
    vn_init();
    p_gemv_int8(m, vec, out, rows, cols);
}
