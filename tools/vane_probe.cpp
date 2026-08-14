/* vane_probe — report what this CPU actually offers and what got bound.
 * Copyright 2026 The Vane Authors. Apache License 2.0. */
#include "vane.h"
#include <cstdio>

int main(void)
{
    if (vn_init() != 0) {
        std::fprintf(stderr, "vn_init failed\n");
        return 1;
    }

    vn_caps_t c;
    vn_get_caps(&c);

    std::printf("vane probe\n");
    std::printf("  cpu             : %s\n", c.cpu_desc[0] ? c.cpu_desc : "(not exposed)");
    std::printf("  os              : %s\n", c.os_desc);
    std::printf("  features        : %s\n", vn_features_string());
    if (c.sve_vector_bits) std::printf("  sve vector len  : %u bits\n", c.sve_vector_bits);
    else                   std::printf("  sve vector len  : n/a\n");
    std::printf("  dispatched to   : %s\n", vn_path_name(vn_active_path()));

    std::printf("\n  available paths :");
    const vn_path_t paths[] = { VN_PATH_SCALAR, VN_PATH_NEON, VN_PATH_SVE };
    const vn_path_t restore = vn_active_path();
    for (vn_path_t p : paths)
        if (vn_force_path(p) == 0) std::printf(" %s", vn_path_name(p));
    vn_force_path(restore);
    std::printf("\n");
    return 0;
}
