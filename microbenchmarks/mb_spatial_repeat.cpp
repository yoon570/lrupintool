// microbench_hot_cold.cpp
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>

#define PAGE_SIZE         4096
#define LINE_SIZE           64   // cache line size you model
#define PROGRESS_STEP_PCT   20   // print bar every 20%
static const int SPATIAL_DEGREE = 8; // number of lines touched per page

static inline void touch_addr(volatile char *p) {
    volatile char v = *p;
    (void)v;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <RSS_in_pages> <total_iterations>\n", argv[0]);
        return 1;
    }

    long rss_pages   = atol(argv[1]);
    long total_iters = atol(argv[2]);
    if (rss_pages <= 0 || total_iters <= 0) {
        fprintf(stderr, "Both arguments must be > 0\n");
        return 1;
    }

    // Split pages: 20% hot, 80% cold
    long hot_pages  = (rss_pages * 20) / 100;
    if (hot_pages < 1)         hot_pages = 1;
    if (hot_pages > rss_pages) hot_pages = rss_pages;
    long cold_pages = rss_pages - hot_pages;
    long hot_start  = cold_pages;

    // Allocate entire region
    size_t region_sz = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char*)malloc(region_sz);
    if (!region) { perror("malloc"); return 1; }

    // Phase 0: fault in every page once
    for (long i = 0; i < rss_pages; ++i) {
        touch_addr(region + (size_t)i * PAGE_SIZE);
    }

    long hot_iters  = (total_iters * 80) / 100;
    long cold_iters = total_iters - hot_iters;

    // Progress
    long done = 0;
    int  last_step = -1;
    auto show_progress = [&]() {
        int pct  = (int)((done * 100) / total_iters);
        int step = pct / PROGRESS_STEP_PCT;
        if (step != last_step && pct % PROGRESS_STEP_PCT == 0) {
            last_step = step;
            printf("%d%% ", pct);
            fflush(stdout);
        }
    };

    // Hammer one page: touch SPATIAL_DEGREE distinct 64B lines
    // Mix page index into the line choice to break set aliasing.
    auto hammer_page = [&](long pg) {
        size_t base = (size_t)pg * PAGE_SIZE;
        const int LINES_PER_PAGE = PAGE_SIZE / LINE_SIZE; // 64

        for (int s = 0; s < SPATIAL_DEGREE; ++s) {
            // Spread across sets: co-prime-ish multipliers with 64
            int line = (int)((pg * 53 + (s + 1) * 17) & (LINES_PER_PAGE - 1)); // 0..63
            size_t offset = (size_t)line * LINE_SIZE;
            touch_addr(region + base + offset);
        }
    };

    // 1) Cold pages (remaining 20%)
    if (cold_pages > 0) {
        for (long i = 0; i < cold_iters; ++i) {
            long page = (i % cold_pages);
            hammer_page(page);
            ++done;
            show_progress();
        }
    }

    // 2) Hot pages (80% of accesses)
    for (long i = 0; i < hot_iters; ++i) {
        long page = hot_start + (i % hot_pages);
        hammer_page(page);
        ++done;
        show_progress();
    }

    putchar('\n');
    free((void*)region);
    return 0;
}

