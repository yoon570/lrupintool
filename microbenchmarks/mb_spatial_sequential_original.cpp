#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>

//#define PAGE_SIZE         64
#define PAGE_SIZE         4096
#define PROGRESS_STEP_PCT 20  /* print bar every 20 % */

/* number of distinct offsets to touch _within_ each page */
static const int SPATIAL_DEGREE = 8;

static inline void touch_addr(volatile char *p)
{
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

    /* split pages: 20% hot, 80% cold */
    long hot_pages  = (rss_pages * 20) / 100;
    if (hot_pages < 1)      hot_pages = 1;
    if (hot_pages > rss_pages) hot_pages = rss_pages;
    long cold_pages = rss_pages - hot_pages;
    long hot_start  = cold_pages;

    /* allocate entire region */
    size_t region_sz = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char*)malloc(region_sz);
    if (!region) { perror("malloc"); return 1; }

    /* Phase 0: fault in every page once */
    for (long i = 0; i < rss_pages; ++i) {
        /* touch the first byte of each page to fault it in */
        touch_addr(region + i * PAGE_SIZE);
    }

    long hot_iters  = (total_iters * 80) / 100;
    long cold_iters = total_iters - hot_iters;

    /* --------------------------------------------------------------------
     * Progress-bar helpers
     * ------------------------------------------------------------------ */
    long done = 0;           /* how many iterations we have completed */
    int  last_step = -1;     /* last PROGRESS_STEP_PCT bucket printed */

    auto show_progress = [&]() {
        int pct = (int)((done * 100) / total_iters);
        int step = pct / PROGRESS_STEP_PCT;
        if (step != last_step && pct % PROGRESS_STEP_PCT == 0) {
            last_step = step;
            printf("%d%% ", pct);
            fflush(stdout);
        }
    };

    /* helper to touch SPATIAL_DEGREE offsets in one page */
    auto hammer_page = [&](long pg) {
        size_t base = (size_t)pg * PAGE_SIZE;
        for (int s = 0; s < SPATIAL_DEGREE; ++s) {
            /* evenly spaced offsets within the page */
            size_t offset = (PAGE_SIZE * s) / SPATIAL_DEGREE;
            touch_addr(region + base + offset);
        }
    };

    /* --------------------------------------------------------------------
     * 1) Hammer COLD pages (remaining 20 %)
     * ------------------------------------------------------------------ */
    for (long i = 0; i < cold_iters; ++i) {
        long page = (i % cold_pages);
        hammer_page(page);
        ++done;
        show_progress();
    }

    /* --------------------------------------------------------------------
     * 2) Hammer HOT pages first (80 % of accesses)
     * ------------------------------------------------------------------ */
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

