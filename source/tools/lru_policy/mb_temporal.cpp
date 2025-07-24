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
    volatile char v = *p; (void)v;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
            "Usage: %s <pages> <iters> <hot_frac(0–1)> <hot_pages>\n",
            argv[0]);
        return 1;
    }
    long pages    = std::atol(argv[1]);    // e.g. 10000
    long iters    = std::atol(argv[2]);    // should be 1000000
    double hot_f  = std::atof(argv[3]);    // e.g. 0.8
    long hot_p    = std::atol(argv[4]);    // e.g. 2000

    if (pages <= 0 || iters <= 0 || hot_f <= 0.0 || hot_f >= 1.0
     || hot_p <= 0 || hot_p > pages) {
        std::fprintf(stderr,"Bad args\n");
        return 1;
    }
    long cold_p = pages - hot_p;

    size_t bytes = (size_t)pages * PAGE_SIZE;
    void*  region = mmap(nullptr, bytes,
                         PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    volatile char* mem = (volatile char*)region;

    for (long p = pages - 1; p > 0; --p)
        touch(mem + p * PAGE_SIZE);

    std::mt19937_64 rng((uint64_t)time(nullptr) ^ (uintptr_t)&rng);
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<long>    hot_idx(0, hot_p - 1);
    std::uniform_int_distribution<long>    cold_idx(0, cold_p - 1);

    for (long i = 0; i < iters; ++i) {
        long pg = (prob(rng) < hot_f)
                  ? hot_idx(rng)
                  : (hot_p + cold_idx(rng));
        touch(mem + pg * PAGE_SIZE);
    }

    munmap(region, bytes);
    return 0;
}
