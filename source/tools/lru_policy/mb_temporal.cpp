// SPDX‑License‑Identifier: MIT
#define _POSIX_C_SOURCE 200809L

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <random>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PAGE_SIZE
#  define PAGE_SIZE 4096
#endif

static inline void touch(volatile char* p) {
    volatile char v = *p;
    (void)v;
}

int main(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr,
            "Usage: %s <pages> <iters> <hot_frac(0–1)> <hot_pages> [seed]\n",
            argv[0]);
        return 1;
    }

    const long    pages   = std::atol(argv[1]);   // e.g. 10000
    const long    iters   = std::atol(argv[2]);   // e.g. 1000000
    const double  hot_f   = std::atof(argv[3]);   // e.g. 0.8 (probability a touch is hot)
    const long    hot_p   = std::atol(argv[4]);   // e.g. 2000 (# of hot pages = last 20 %)
    const long    cold_p  = pages - hot_p;
    const uint64_t seed   = (argc == 6)
                          ? static_cast<uint64_t>(std::strtoull(argv[5], nullptr, 10))
                          : 0x5EEDULL;            // deterministic default

    if (pages <= 0 || iters <= 0 ||
        hot_f  <= 0.0 || hot_f >= 1.0 ||
        hot_p  <= 0   || hot_p > pages) {
        std::fprintf(stderr, "Bad args\n");
        return 1;
    }

    const size_t bytes = static_cast<size_t>(pages) * PAGE_SIZE;
    void* region = mmap(nullptr, bytes,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    volatile char* const mem = static_cast<volatile char*>(region);

    // ── 1) Fault‑in every page sequentially (improves locality) ──────────────
    for (long p = 0; p < pages; ++p) {
        touch(mem + p * PAGE_SIZE);
    }

    // ── 2) RNGs seeded deterministically ─────────────────────────────────────
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<long>    hot_sel(0,  hot_p  - 1);
    std::uniform_int_distribution<long>    cold_sel(0, cold_p - 1);

    const long hot_base  = pages - hot_p;   // first page index of the hot region

    long hot_cnt = 0, cold_cnt = 0;

    // ── 3) Main touch loop ──────────────────────────────────────────────────
    for (long i = 0; i < iters; ++i) {
        bool is_hot = prob(rng) < hot_f;
        long pg = is_hot
                ? hot_base + hot_sel(rng)
                : cold_sel(rng);
        touch(mem + pg * PAGE_SIZE);
        if (is_hot) ++hot_cnt; else ++cold_cnt;
    }

    std::printf("[stats] hot %ld  cold %ld\n", hot_cnt, cold_cnt);

    munmap(region, bytes);
    return 0;
}
