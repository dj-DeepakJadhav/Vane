/* vane_bench — measure every available path on this device.
 *
 * Speedups printed here are ratios of medians taken from live timers on this
 * machine in this run. Nothing is modelled. If a path is unavailable it is
 * reported as unavailable rather than estimated.
 *
 * Usage: vane_bench [--seconds N] [--rows N] [--cols N] [--report FILE]
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "vane.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {

void print_row(const vn_bench_result_t &r, double baseline_median)
{
    std::printf("  %-8s %10.0f %10.0f %9.2f %9lld",
                r.path, r.median_ns, r.p95_ns, r.gb_per_sec,
                (long long)r.iterations);

    if (baseline_median > 0.0 && r.median_ns > 0.0)
        std::printf(" %8.2fx", baseline_median / r.median_ns);
    else
        std::printf("        --");

    if (r.sustained_ratio != VN_UNMEASURED) std::printf(" %9.2f", r.sustained_ratio);
    else                                    std::printf("         -");

    if (r.thermal_c_end != VN_UNMEASURED)   std::printf(" %8.1f\n", r.thermal_c_end);
    else                                    std::printf("        -\n");
}

void header(const char *op, int rows, int cols)
{
    std::printf("\n%s  (%d x %d)\n", op, rows, cols);
    std::printf("  %-8s %10s %10s %9s %9s %9s %9s %8s\n",
                "path", "median_ns", "p95_ns", "GB/s", "iters",
                "vs scalar", "sustained", "temp_C");
    std::printf("  %s\n", "----------------------------------------------------------------------------------------");
}

} /* namespace */

int main(int argc, char **argv)
{
    double seconds = 3.0;
    int rows = 512, cols = 4096;
    const char *report_path = nullptr;
    const char *only_op = nullptr;

    static const char *kAllOps[] = { "dequant_gemv_int4", "gemv_int8", "quantize_int4" };

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc) cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report_path = argv[++i];
        else if (!strcmp(argv[i], "--op") && i + 1 < argc) only_op = argv[++i];
        else {
            std::printf("usage: %s [--seconds N] [--rows N] [--cols N] [--op NAME] [--report FILE]\n",
                        argv[0]);
            std::printf("  --op   one of: dequant_gemv_int4 | gemv_int8 | quantize_int4\n");
            std::printf("         omit to run all three\n");
            return 2;
        }
    }
    if (only_op) {
        bool known = false;
        for (const char *o : kAllOps) if (!strcmp(o, only_op)) known = true;
        if (!known) {
            std::fprintf(stderr, "unknown op '%s'\n", only_op);
            return 2;
        }
    }
    if (cols % VN_BLOCK_SIZE != 0) {
        std::fprintf(stderr, "cols must be a multiple of %d\n", VN_BLOCK_SIZE);
        return 2;
    }

    vn_init();
    vn_caps_t c;
    vn_get_caps(&c);

    std::printf("vane_bench\n");
    std::printf("  cpu      : %s\n", c.cpu_desc[0] ? c.cpu_desc : "(not exposed)");
    std::printf("  os       : %s\n", c.os_desc);
    std::printf("  features : %s\n", vn_features_string());
    if (c.sve_vector_bits) std::printf("  sve vl   : %u bits\n", c.sve_vector_bits);
    std::printf("  window   : %.1f s per path\n", seconds);
    if (vn_read_thermal_c() == VN_UNMEASURED)
        std::printf("  thermal  : no readable sensor on this platform (reported as null)\n");

    std::vector<vn_bench_result_t> all;
    const vn_path_t paths[] = { VN_PATH_SCALAR, VN_PATH_NEON, VN_PATH_SVE };

    for (const char *op : kAllOps) {
        if (only_op && strcmp(op, only_op) != 0) continue;
        header(op, rows, cols);
        double baseline = 0.0;
        for (vn_path_t p : paths) {
            if (vn_force_path(p) != 0) {
                std::printf("  %-8s %s\n", vn_path_name(p), "unavailable on this CPU");
                continue;
            }
            vn_bench_result_t r = vn_bench(op, rows, cols, seconds);
            if (p == VN_PATH_SCALAR) baseline = r.median_ns;
            print_row(r, baseline);
            all.push_back(r);
        }
    }

    vn_force_path(VN_PATH_AUTO);
    std::printf("\n  auto-dispatch selects: %s\n", vn_path_name(vn_active_path()));
    std::printf("  sustained = (median of first 20%% of run) / (median of last 20%%);"
                " below 1.00 means throughput fell during the run\n");

    if (report_path) {
        if (vn_write_report(report_path, all.data(), (int)all.size()) == 0)
            std::printf("  report written: %s\n", report_path);
        else
            std::fprintf(stderr, "  failed to write report: %s\n", report_path);
    }
    return 0;
}
