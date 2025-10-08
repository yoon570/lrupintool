#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
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
    if (hot_pages < 1)         hot_pages = 1;
    if (hot_pages > rss_pages) hot_pages = rss_pages;
    long cold_pages = rss_pages - hot_pages;
    long hot_start  = cold_pages;

    /* allocate entire region */
    size_t region_sz = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char*)malloc(region_sz);
    if (!region) { perror("malloc"); return 1; }

    /* Compute base addresses and page-number ranges */
    uintptr_t base = (uintptr_t)region;

    /* Byte ranges (original) */
    uintptr_t cold_start_addr     = base;
    uintptr_t cold_end_addr_excl  = base + (uintptr_t)cold_pages * PAGE_SIZE;
    uintptr_t hot_start_addr      = base + (uintptr_t)hot_start * PAGE_SIZE;
    uintptr_t hot_end_addr_excl   = base + (uintptr_t)rss_pages * PAGE_SIZE;

    /* 4KiB virtual page numbers (VPN4K = addr >> 12) */
    uint64_t vpn4k_base           = (uint64_t)(base >> 12);
    uint64_t cold_start_vpn4k     = vpn4k_base;
    uint64_t cold_end_vpn4k_excl  = vpn4k_base + (uint64_t)cold_pages;
    uint64_t hot_start_vpn4k      = vpn4k_base + (uint64_t)hot_start;
    uint64_t hot_end_vpn4k_excl   = vpn4k_base + (uint64_t)rss_pages;

    /* 64B “line” numbers (VP64 = addr >> 6), useful if you want HashLL units */
    uint64_t vp64_base            = (uint64_t)(base >> 6);
    uint64_t cold_start_vp64      = vp64_base;
    uint64_t cold_end_vp64_excl   = vp64_base + ((uint64_t)cold_pages * (PAGE_SIZE / 64));
    uint64_t hot_start_vp64       = vp64_base + ((uint64_t)hot_start * (PAGE_SIZE / 64));
    uint64_t hot_end_vp64_excl    = vp64_base + ((uint64_t)rss_pages * (PAGE_SIZE / 64));

    /* Pretty print */
    printf("=== Layout ===\n");
    printf("Total pages: %ld  (bytes: %zu)\n", rss_pages, region_sz);

    /* Byte address ranges (inclusive, exclusive) */
    printf("\n[Byte address ranges]\n");
    if (cold_pages > 0) {
        printf("Cold: [0x%016" PRIxPTR ", 0x%016" PRIxPTR ")\n",
               cold_start_addr, cold_end_addr_excl);
    } else {
        printf("Cold: (none)\n");
    }
    printf("Hot : [0x%016" PRIxPTR ", 0x%016" PRIxPTR ")\n",
           hot_start_addr, hot_end_addr_excl);

    /* 4KiB virtual page ranges (VPN4K) */
    printf("\n[4KiB virtual page numbers (VPN4K = addr>>12)]\n");
    if (cold_pages > 0) {
        printf("Cold VPN4K: [%" PRIu64 ", %" PRIu64 ")\n",
               cold_start_vpn4k, cold_end_vpn4k_excl);
    } else {
        printf("Cold VPN4K: (none)\n");
    }
    printf("Hot  VPN4K: [%" PRIu64 ", %" PRIu64 ")\n",
           hot_start_vpn4k, hot_end_vpn4k_excl);

    /* 64B line-number ranges (VP64) — matches HashLL granularity */
    printf("\n[64B line numbers (VP64 = addr>>6) — HashLL units]\n");
    if (cold_pages > 0) {
        printf("Cold VP64:  [%" PRIu64 ", %" PRIu64 ")\n",
               cold_start_vp64, cold_end_vp64_excl);
    } else {
        printf("Cold VP64:  (none)\n");
    }
    printf("Hot  VP64:  [%" PRIu64 ", %" PRIu64 ")\n",
           hot_start_vp64, hot_end_vp64_excl);

    /* Phase 0: fault in every page once */
    for (long i = 0; i < rss_pages; ++i) {
        touch_addr(region + i * PAGE_SIZE);
    }

    long hot_iters  = (total_iters * 80) / 100;
    long cold_iters = total_iters - hot_iters;

    /* Progress-bar helpers */
    long done = 0;
    int  last_step = -1;

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
        size_t base_off = (size_t)pg * PAGE_SIZE;
        for (int s = 0; s < SPATIAL_DEGREE; ++s) {
            size_t offset = (PAGE_SIZE * s) / SPATIAL_DEGREE;
            touch_addr(region + base_off + offset);
        }
    };

    /* 1) Hammer COLD pages (20%) */
    for (long i = 0; i < cold_iters; ++i) {
        if (cold_pages > 0) {
            long page = (i % cold_pages);
            hammer_page(page);
        }
        ++done;
        show_progress();
    }

    /* 2) Hammer HOT pages (80%) */
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
