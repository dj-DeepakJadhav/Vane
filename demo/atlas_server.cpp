/* atlas_server — semantic search over INT4 word vectors, computed on Arm.
 *
 * The browser draws. This process computes. Every similarity score the page
 * displays came out of vn_dequant_gemv_int4 running on this CPU, and the
 * microsecond figure next to it came off the same monotonic clock the
 * benchmark uses. There is no arithmetic in the JavaScript.
 *
 * That direction matters. A web page cannot execute NEON or SVE, so any demo
 * claiming otherwise is theatre. Here the page is a display for work the Arm
 * core actually did.
 *
 *   ./atlas_server --data demo/atlas.bin --html demo/atlas.html --port 8080
 *
 * Copyright 2026 The Vane Authors. Apache License 2.0.
 */
#include "vane.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#else
#  error "atlas_server targets POSIX sockets (Linux, Android, macOS)"
#endif

namespace {

using clk = std::chrono::steady_clock;

/* ----------------------------------------------------------------- corpus */

struct Atlas {
    uint32_t n = 0, dim = 0, nblocks = 0, dim_raw = 0;
    std::vector<uint8_t> packed;
    std::vector<float>   scales;
    std::vector<float>   coords;      /* 2 per word */
    std::vector<std::string> vocab;
    std::unordered_map<std::string, uint32_t> index;

    size_t int4_bytes() const { return packed.size() + scales.size() * 4; }

    /* Against the SOURCE data, not against our zero-padded working copy.
     * Padding 300 dims up to 320 inflates the baseline and would report
     * 84.4% where the honest figure is 83.3%. */
    size_t fp32_bytes() const { return (size_t)n * dim_raw * 4; }

    /* The padded matrix, kept only so the startup banner can be explicit
     * about which comparison it is making. */
    size_t fp32_padded_bytes() const { return (size_t)n * dim * 4; }
};

bool load_atlas(const char *path, Atlas &a)
{
    FILE *f = fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }

    char magic[8];
    uint32_t vocab_len = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "VNATLAS2", 8) != 0) {
        std::fprintf(stderr, "%s: bad magic (expected VNATLAS2; repack with "
                             "demo/atlas_pack.py)\n", path);
        fclose(f); return false;
    }
    if (fread(&a.n, 4, 1, f) != 1 || fread(&a.dim, 4, 1, f) != 1 ||
        fread(&a.nblocks, 4, 1, f) != 1 || fread(&a.dim_raw, 4, 1, f) != 1 ||
        fread(&vocab_len, 4, 1, f) != 1) {
        fclose(f); return false;
    }
    if (a.dim_raw == 0 || a.dim_raw > a.dim) {
        std::fprintf(stderr, "%s: implausible dim_raw %u (dim %u)\n",
                     path, a.dim_raw, a.dim);
        fclose(f); return false;
    }

    a.packed.resize((size_t)a.n * a.dim / 2);
    a.scales.resize((size_t)a.n * a.nblocks);
    a.coords.resize((size_t)a.n * 2);
    std::string blob(vocab_len, '\0');

    bool ok = fread(a.packed.data(), 1, a.packed.size(), f) == a.packed.size()
           && fread(a.scales.data(), 4, a.scales.size(), f) == a.scales.size()
           && fread(a.coords.data(), 4, a.coords.size(), f) == a.coords.size()
           && fread(&blob[0], 1, vocab_len, f) == vocab_len;
    fclose(f);
    if (!ok) { std::fprintf(stderr, "%s: truncated\n", path); return false; }

    a.vocab.reserve(a.n);
    size_t start = 0;
    while (start < blob.size() && a.vocab.size() < a.n) {
        size_t nl = blob.find('\n', start);
        if (nl == std::string::npos) break;
        a.vocab.emplace_back(blob.substr(start, nl - start));
        start = nl + 1;
    }
    for (uint32_t i = 0; i < a.vocab.size(); ++i) a.index[a.vocab[i]] = i;
    return a.vocab.size() == a.n;
}

/* Dequantise one row into a full fp32 vector, for use as a query. */
void row_vector(const Atlas &a, uint32_t row, std::vector<float> &out)
{
    out.assign(a.dim, 0.0f);
    vn_dequantize_int4(a.packed.data() + (size_t)row * a.dim / 2,
                       a.scales.data() + (size_t)row * a.nblocks,
                       out.data(), (int)a.dim);
}

struct Hit { uint32_t idx; float score; };

struct Timing { double median_us, min_us; int reps; };

/* The search itself: one dispatched kernel call over the whole corpus,
 * then a partial sort.
 *
 * WHY THIS TIMES A REPEATED CALL
 * ------------------------------
 * A phone's scheduler migrates a process between big and little cores between
 * one HTTP request and the next, which on this hardware moves a single
 * measurement by more than 4x — far more than the difference between the
 * kernels being compared. Timing once would report the scheduler, not the
 * code. So the kernel is run `reps` times and the median reported, which is
 * the same method vane_bench uses over thousands of iterations, at a scale
 * that still fits inside an interactive request.
 *
 * The minimum is reported alongside: it is the closest thing to an
 * uninterrupted run, and the gap between min and median is itself the honest
 * measure of how much the machine is interfering. */
Timing search(const Atlas &a, const std::vector<float> &query, int k,
              std::vector<Hit> &hits, int reps)
{
    std::vector<float> scores(a.n);
    std::vector<double> samples;
    samples.reserve((size_t)reps);

    for (int i = 0; i < reps; ++i) {
        const auto t0 = clk::now();
        vn_dequant_gemv_int4(a.packed.data(), a.scales.data(), query.data(),
                             scores.data(), (int)a.n, (int)a.dim);
        const auto t1 = clk::now();
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    const Timing t{ samples[samples.size() / 2], samples.front(), reps };

    if (k > (int)a.n) k = (int)a.n;
    std::vector<uint32_t> idx(a.n);
    for (uint32_t i = 0; i < a.n; ++i) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](uint32_t x, uint32_t y) { return scores[x] > scores[y]; });

    hits.clear();
    for (int i = 0; i < k; ++i) hits.push_back({ idx[i], scores[idx[i]] });
    return t;
}

/* -------------------------------------------------------------- utilities */

std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query(const std::string &q)
{
    std::unordered_map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        size_t eq = q.find('=', pos);
        if (eq != std::string::npos && eq < amp)
            kv[q.substr(pos, eq - pos)] = url_decode(q.substr(eq + 1, amp - eq - 1));
        pos = amp + 1;
    }
    return kv;
}

std::string json_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o.push_back(c);
    }
    return o;
}

void send_all(int fd, const char *p, size_t n)
{
    while (n) { ssize_t w = send(fd, p, n, 0); if (w <= 0) return; p += w; n -= (size_t)w; }
}

void respond(int fd, const char *status, const char *ctype,
             const char *body, size_t len)
{
    char hdr[512];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", status, ctype, len);
    send_all(fd, hdr, (size_t)h);
    if (len) send_all(fd, body, len);
}

void respond(int fd, const char *status, const char *ctype, const std::string &b)
{
    respond(fd, status, ctype, b.data(), b.size());
}

vn_path_t path_from_name(const std::string &s)
{
    if (s == "scalar") return VN_PATH_SCALAR;
    if (s == "neon")   return VN_PATH_NEON;
    if (s == "sve")    return VN_PATH_SVE;
    return VN_PATH_AUTO;
}

/* ------------------------------------------------------------------ state */

Atlas       g_atlas;
std::string g_html;

/* Which paths this CPU can actually execute. Probed by asking the dispatcher
 * to bind each one and restoring the previous selection afterwards, so the UI
 * offers only paths that will really run rather than advertising a button
 * that silently falls back. */
std::string available_paths_json()
{
    const vn_path_t restore = vn_active_path();
    const vn_path_t all[] = { VN_PATH_SCALAR, VN_PATH_NEON, VN_PATH_SVE };

    std::string j = "[";
    for (vn_path_t p : all) {
        if (vn_force_path(p) != 0) continue;
        if (j.size() > 1) j += ",";
        j += "\"" + std::string(vn_path_name(p)) + "\"";
    }
    vn_force_path(restore);
    return j + "]";
}

std::string caps_json()
{
    vn_caps_t c; vn_get_caps(&c);
    const std::string paths = available_paths_json();
    char buf[1400];
    snprintf(buf, sizeof buf,
        "{\"features\":\"%s\",\"cpu\":\"%s\",\"os\":\"%s\","
        "\"sve_vector_bits\":%s,\"path\":\"%s\",\"available_paths\":%s,"
        "\"words\":%u,\"dim\":%u,"
        "\"int4_bytes\":%zu,\"fp32_bytes\":%zu}",   /* fp32_bytes = source, unpadded */
        vn_features_string(),
        json_escape(c.cpu_desc[0] ? c.cpu_desc : "not exposed").c_str(),
        c.os_desc,
        c.sve_vector_bits ? std::to_string(c.sve_vector_bits).c_str() : "null",
        vn_path_name(vn_active_path()),
        paths.c_str(),
        g_atlas.n, g_atlas.dim,
        g_atlas.int4_bytes(), g_atlas.fp32_bytes());
    return buf;
}

std::string hits_json(const std::vector<Hit> &hits, const Timing &t,
                      const std::string &label)
{
    std::string j = "{\"query\":\"" + json_escape(label) + "\",";
    j += "\"path\":\"" + std::string(vn_path_name(vn_active_path())) + "\",";
    j += "\"kernel_us\":" + std::to_string(t.median_us) + ",";
    j += "\"kernel_min_us\":" + std::to_string(t.min_us) + ",";
    j += "\"reps\":" + std::to_string(t.reps) + ",";
    j += "\"results\":[";
    for (size_t i = 0; i < hits.size(); ++i) {
        const uint32_t x = hits[i].idx;
        char b[256];
        snprintf(b, sizeof b,
                 "%s{\"word\":\"%s\",\"score\":%.4f,\"x\":%.4f,\"y\":%.4f}",
                 i ? "," : "", json_escape(g_atlas.vocab[x]).c_str(),
                 hits[i].score, g_atlas.coords[x * 2], g_atlas.coords[x * 2 + 1]);
        j += b;
    }
    return j + "]}";
}

void handle(int fd)
{
    char req[4096];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) { close(fd); return; }
    req[n] = '\0';

    const char *sp = strchr(req, ' ');
    if (!sp) { close(fd); return; }
    const char *end = strchr(sp + 1, ' ');
    if (!end) { close(fd); return; }
    std::string target(sp + 1, end);

    std::string path = target, qs;
    size_t qm = target.find('?');
    if (qm != std::string::npos) { path = target.substr(0, qm); qs = target.substr(qm + 1); }
    auto args = parse_query(qs);

    if (path == "/" || path == "/index.html") {
        respond(fd, "200 OK", "text/html; charset=utf-8", g_html);
    }
    else if (path == "/api/caps") {
        respond(fd, "200 OK", "application/json", caps_json());
    }
    else if (path == "/api/vocab") {
        std::string v;
        v.reserve(g_atlas.n * 8);
        for (const auto &w : g_atlas.vocab) { v += w; v.push_back('\n'); }
        respond(fd, "200 OK", "text/plain; charset=utf-8", v);
    }
    else if (path == "/api/coords") {
        respond(fd, "200 OK", "application/octet-stream",
                (const char *)g_atlas.coords.data(), g_atlas.coords.size() * 4);
    }
    else if (path == "/api/compare") {
        /* A/B two kernels under identical conditions.
         *
         * Timing them in separate HTTP requests does not work on a phone: the
         * scheduler places each request on a big or little core independently,
         * and that choice moves the result by more than the kernels differ
         * from each other. Measured separately, NEON can appear slower than
         * scalar purely because it landed on a little core.
         *
         * So both paths are measured here, alternating, inside one request.
         * They then share the same core, the same thermal state and the same
         * cache contents, and the ratio between them means something. */
        auto it = g_atlas.index.find(args.count("q") ? args["q"] : "");
        if (it == g_atlas.index.end()) {
            respond(fd, "404 Not Found", "application/json",
                    "{\"error\":\"word not in vocabulary\"}");
            close(fd); return;
        }
        const int reps = args.count("reps")
                       ? std::min(51, std::max(3, atoi(args["reps"].c_str()))) : 15;

        std::vector<float> q;
        row_vector(g_atlas, it->second, q);
        std::vector<float> scores(g_atlas.n);

        const vn_path_t restore = vn_active_path();
        std::vector<double> s_samples, n_samples;

        for (int i = 0; i < reps; ++i) {
            for (int which = 0; which < 2; ++which) {
                const vn_path_t p = which ? VN_PATH_NEON : VN_PATH_SCALAR;
                if (vn_force_path(p) != 0) continue;
                const auto t0 = clk::now();
                vn_dequant_gemv_int4(g_atlas.packed.data(), g_atlas.scales.data(),
                                     q.data(), scores.data(),
                                     (int)g_atlas.n, (int)g_atlas.dim);
                const auto t1 = clk::now();
                const double us =
                    std::chrono::duration<double, std::micro>(t1 - t0).count();
                (which ? n_samples : s_samples).push_back(us);
            }
        }
        vn_force_path(restore);

        auto med = [](std::vector<double> v) {
            if (v.empty()) return -1.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double sm = med(s_samples), nm = med(n_samples);

        char buf[512];
        snprintf(buf, sizeof buf,
            "{\"query\":\"%s\",\"reps\":%d,\"interleaved\":true,"
            "\"scalar_us\":%.1f,\"neon_us\":%.1f,\"speedup\":%s,"
            "\"scalar_min_us\":%.1f,\"neon_min_us\":%.1f}",
            json_escape(args["q"]).c_str(), reps, sm, nm,
            (sm > 0 && nm > 0) ? std::to_string(sm / nm).c_str() : "null",
            s_samples.empty() ? -1.0 : *std::min_element(s_samples.begin(), s_samples.end()),
            n_samples.empty() ? -1.0 : *std::min_element(n_samples.begin(), n_samples.end()));
        respond(fd, "200 OK", "application/json", buf);
    }
    else if (path == "/api/search" || path == "/api/analogy") {
        if (args.count("path")) vn_force_path(path_from_name(args["path"]));

        const int k = args.count("k") ? std::max(1, atoi(args["k"].c_str())) : 20;
        /* Bounded so a crafted request cannot pin the device. */
        const int reps = args.count("reps")
                       ? std::min(51, std::max(1, atoi(args["reps"].c_str()))) : 7;
        std::vector<float> query(g_atlas.dim, 0.0f), tmp;
        std::string label;
        bool ok = true;

        auto fetch = [&](const std::string &w, std::vector<float> &dst) {
            auto it = g_atlas.index.find(w);
            if (it == g_atlas.index.end()) return false;
            row_vector(g_atlas, it->second, dst);
            return true;
        };

        if (path == "/api/search") {
            label = args.count("q") ? args["q"] : "";
            ok = fetch(label, query);
        } else {
            /* a - b + c, the classic analogy form. */
            const std::string a = args["a"], b = args["b"], c = args["c"];
            label = a + " - " + b + " + " + c;
            std::vector<float> va, vb, vc;
            ok = fetch(a, va) && fetch(b, vb) && fetch(c, vc);
            if (ok) {
                for (uint32_t i = 0; i < g_atlas.dim; ++i) query[i] = va[i] - vb[i] + vc[i];
                double nrm = 0.0;
                for (float v : query) nrm += (double)v * v;
                nrm = std::sqrt(nrm);
                if (nrm > 0) for (auto &v : query) v = (float)(v / nrm);
            }
        }

        if (!ok) {
            respond(fd, "404 Not Found", "application/json",
                    std::string("{\"error\":\"word not in vocabulary\",\"query\":\"")
                    + json_escape(label) + "\"}");
        } else {
            std::vector<Hit> hits;
            const Timing t = search(g_atlas, query, k, hits, reps);
            respond(fd, "200 OK", "application/json", hits_json(hits, t, label));
        }
    }
    else {
        respond(fd, "404 Not Found", "text/plain", "not found");
    }
    close(fd);
}

std::string read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return {};
    std::string s;
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, r);
    fclose(f);
    return s;
}

} /* namespace */

int main(int argc, char **argv)
{
    const char *data = "atlas.bin";
    const char *html = "atlas.html";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--html") && i + 1 < argc) html = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else { std::printf("usage: %s [--data F] [--html F] [--port N]\n", argv[0]); return 2; }
    }

    vn_init();
    if (!load_atlas(data, g_atlas)) return 1;
    g_html = read_file(html);
    if (g_html.empty()) {
        std::fprintf(stderr, "warning: %s missing or empty; API still served\n", html);
        g_html = "<!doctype html><p>atlas.html not found next to the binary.";
    }

    /* Touch the corpus once before serving. The first query over a freshly
     * loaded 1 MB matrix runs several times slower purely from cold caches,
     * which would make the very first number a visitor sees unrepresentative
     * of the kernel. This is a cache effect, not a correction factor: the
     * timing reported per request is still the real elapsed time of that
     * request. */
    {
        std::vector<float> warm_q(g_atlas.dim, 0.01f), warm_out(g_atlas.n);
        for (int i = 0; i < 3; ++i)
            vn_dequant_gemv_int4(g_atlas.packed.data(), g_atlas.scales.data(),
                                 warm_q.data(), warm_out.data(),
                                 (int)g_atlas.n, (int)g_atlas.dim);
    }

    vn_caps_t c; vn_get_caps(&c);
    std::printf("atlas: %u words x %u dims (source %u, padded to a multiple of 32)\n",
                g_atlas.n, g_atlas.dim, g_atlas.dim_raw);
    std::printf("  int4 %.2f MB vs source fp32 %.2f MB (%.1f%% smaller)\n",
                g_atlas.int4_bytes() / 1e6, g_atlas.fp32_bytes() / 1e6,
                100.0 * (1.0 - (double)g_atlas.int4_bytes() / (double)g_atlas.fp32_bytes()));
    if (g_atlas.dim != g_atlas.dim_raw)
        std::printf("  (against our padded copy it would read %.2f MB / %.1f%% — "
                    "the line above is the honest one)\n",
                    g_atlas.fp32_padded_bytes() / 1e6,
                    100.0 * (1.0 - (double)g_atlas.int4_bytes()
                                   / (double)g_atlas.fp32_padded_bytes()));
    std::printf("  features   : %s\n", vn_features_string());
    std::printf("  dispatched : %s\n", vn_path_name(vn_active_path()));

    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    std::printf("  listening  : http://0.0.0.0:%d\n", port);
    std::fflush(stdout);

    for (;;) {
        const int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread(handle, fd).detach();
    }
}
