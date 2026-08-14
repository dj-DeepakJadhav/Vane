/* Measurement.
 *
 * The governing rule of this file: every number it emits comes from a live
 * monotonic clock or a sensor read on this machine. There is no modelling,
 * no extrapolation and no fallback constant. Where a platform exposes no
 * thermal sensor, the field is VN_UNMEASURED and serialises as JSON null.
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "vane_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#  include <dirent.h>
#endif

using clk = std::chrono::steady_clock;

/* ------------------------------------------------------------- thermal */

double vn_read_thermal_c(void)
{
#if defined(__linux__) || defined(__ANDROID__)
    /* Report the hottest readable zone. Kernel exposes millidegrees C.
     * Many Android devices restrict these to root; unreadable means
     * unmeasured, never estimated. */
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return VN_UNMEASURED;

    double hottest = VN_UNMEASURED;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        char path[320];
        snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        long milli = 0;
        if (fscanf(f, "%ld", &milli) == 1) {
            const double c = milli / 1000.0;
            /* Zones report in either millidegrees or degrees depending on
             * vendor; reject values outside a physically plausible range
             * rather than guess a scale factor. */
            if (c > 0.0 && c < 150.0 && c > hottest) hottest = c;
        }
        fclose(f);
    }
    closedir(d);
    return hottest;
#else
    return VN_UNMEASURED;
#endif
}

/* ----------------------------------------------------------- benchmark */

namespace {

struct Workload {
    std::vector<uint8_t> q;
    std::vector<float>   scales, vec, out, src;
    std::vector<int8_t>  m8, v8;
    std::vector<int32_t> o32;
    double bytes_per_call = 0.0;
};

void prepare(Workload &w, const char *op, int rows, int cols)
{
    /* Deterministic pseudo-random input. A fixed seed keeps runs on
     * different devices comparable; real values matter because an all-zero
     * buffer would let the memory subsystem behave unrepresentatively. */
    uint32_t s = 0x9E3779B9u;
    auto next = [&s]() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)((int32_t)s) / 2147483648.0f;
    };

    if (!strcmp(op, "gemv_int8")) {
        w.m8.resize((size_t)rows * cols);
        w.v8.resize(cols);
        w.o32.resize(rows);
        for (auto &x : w.m8) x = (int8_t)(next() * 127.0f);
        for (auto &x : w.v8) x = (int8_t)(next() * 127.0f);
        w.bytes_per_call = (double)rows * cols + cols + rows * 4.0;
        return;
    }

    w.src.resize((size_t)rows * cols);
    for (auto &x : w.src) x = next();
    w.q.resize((size_t)rows * cols / 2);
    w.scales.resize((size_t)rows * cols / VN_BLOCK_SIZE);
    w.vec.resize(cols);
    w.out.resize(rows);
    for (int i = 0; i < cols; ++i) w.vec[i] = next();
    vn_quantize_int4(w.src.data(), w.q.data(), w.scales.data(), rows * cols);

    if (!strcmp(op, "quantize_int4"))
        w.bytes_per_call = (double)rows * cols * 4.0 + rows * cols / 2.0;
    else
        w.bytes_per_call = (double)rows * cols / 2.0                       /* packed */
                         + (double)rows * cols / VN_BLOCK_SIZE * 4.0       /* scales */
                         + (double)cols * 4.0 + (double)rows * 4.0;        /* vec+out */
}

inline void run_once(Workload &w, const char *op, int rows, int cols)
{
    if (!strcmp(op, "gemv_int8"))
        vn_gemv_int8(w.m8.data(), w.v8.data(), w.o32.data(), rows, cols);
    else if (!strcmp(op, "quantize_int4"))
        vn_quantize_int4(w.src.data(), w.q.data(), w.scales.data(), rows * cols);
    else
        vn_dequant_gemv_int4(w.q.data(), w.scales.data(), w.vec.data(),
                             w.out.data(), rows, cols);
}

} /* namespace */

vn_bench_result_t vn_bench(const char *op, int rows, int cols, double seconds)
{
    vn_init();

    vn_bench_result_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.op,   sizeof r.op,   "%s", op);
    snprintf(r.path, sizeof r.path, "%s", vn_path_name(vn_active_path()));
    r.rows = rows;
    r.cols = cols;
    r.thermal_c_start = vn_read_thermal_c();
    r.thermal_c_end   = VN_UNMEASURED;
    r.sustained_ratio = VN_UNMEASURED;

    Workload w;
    prepare(w, op, rows, cols);

    /* Warm caches and let any frequency ramp settle before timing. */
    for (int i = 0; i < 3; ++i) run_once(w, op, rows, cols);

    std::vector<double> samples;
    samples.reserve(4096);

    const auto t0 = clk::now();
    const auto deadline = t0 + std::chrono::duration_cast<clk::duration>(
                                   std::chrono::duration<double>(seconds));
    while (clk::now() < deadline) {
        const auto a = clk::now();
        run_once(w, op, rows, cols);
        const auto b = clk::now();
        samples.push_back(std::chrono::duration<double, std::nano>(b - a).count());
    }
    const auto t1 = clk::now();

    r.thermal_c_end = vn_read_thermal_c();
    r.iterations    = (int64_t)samples.size();
    if (samples.empty()) return r;

    /* Sustained behaviour: compare the first fifth of the run against the
     * last fifth. Below two seconds the windows are too short to mean
     * anything, so the field stays unmeasured rather than misleading. */
    if (seconds >= 2.0 && samples.size() >= 25) {
        const size_t win = samples.size() / 5;
        double early = 0.0, late = 0.0;
        for (size_t i = 0; i < win; ++i) early += samples[i];
        for (size_t i = samples.size() - win; i < samples.size(); ++i) late += samples[i];
        if (late > 0.0) r.sustained_ratio = early / late;   /* <1.0 means it slowed */
    }

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    r.median_ns = sorted[sorted.size() / 2];
    r.p95_ns    = sorted[(size_t)((double)(sorted.size() - 1) * 0.95)];
    r.min_ns    = sorted.front();

    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    r.gb_per_sec = (w.bytes_per_call * (double)samples.size()) / elapsed_s / 1e9;
    return r;
}

/* -------------------------------------------------------------- report */

static void json_num(FILE *f, const char *key, double v, int last)
{
    if (v == VN_UNMEASURED) fprintf(f, "      \"%s\": null%s\n",  key, last ? "" : ",");
    else                    fprintf(f, "      \"%s\": %.4f%s\n",  key, v, last ? "" : ",");
}

int vn_write_report(const char *path, const vn_bench_result_t *results, int count)
{
    vn_init();
    vn_caps_t caps;
    vn_get_caps(&caps);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": \"vane.report/1\",\n");
    fprintf(f, "  \"note\": \"Every value is measured on the reporting device. "
               "Fields the platform could not observe are null, never estimated.\",\n");
    fprintf(f, "  \"device\": {\n");
    fprintf(f, "    \"cpu\": \"%s\",\n", caps.cpu_desc[0] ? caps.cpu_desc : "unknown");
    fprintf(f, "    \"os\": \"%s\",\n",  caps.os_desc);
    fprintf(f, "    \"features\": \"%s\",\n", vn_features_string());
    if (caps.sve_vector_bits) fprintf(f, "    \"sve_vector_bits\": %u\n", caps.sve_vector_bits);
    else                      fprintf(f, "    \"sve_vector_bits\": null\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"results\": [\n");

    for (int i = 0; i < count; ++i) {
        const vn_bench_result_t *r = &results[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"op\": \"%s\",\n",   r->op);
        fprintf(f, "      \"path\": \"%s\",\n", r->path);
        fprintf(f, "      \"rows\": %d,\n",     r->rows);
        fprintf(f, "      \"cols\": %d,\n",     r->cols);
        fprintf(f, "      \"iterations\": %lld,\n", (long long)r->iterations);
        json_num(f, "median_ns",       r->median_ns,       0);
        json_num(f, "p95_ns",          r->p95_ns,          0);
        json_num(f, "min_ns",          r->min_ns,          0);
        json_num(f, "gb_per_sec",      r->gb_per_sec,      0);
        json_num(f, "thermal_c_start", r->thermal_c_start, 0);
        json_num(f, "thermal_c_end",   r->thermal_c_end,   0);
        json_num(f, "sustained_ratio", r->sustained_ratio, 1);
        fprintf(f, "    }%s\n", i + 1 < count ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}
