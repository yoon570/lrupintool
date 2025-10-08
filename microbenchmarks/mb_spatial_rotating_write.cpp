// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#define PAGE_SIZE         4096
#define PROGRESS_STEP_PCT 20   /* print progress every 20% */
static const int SPATIAL_DEGREE = 8;

/* Pure WRITE (store) to memory: volatile store => counts as a write access */
static inline void write_addr(volatile unsigned char *p, unsigned char val) {
    *p = val;
}

/* hammer SPATIAL_DEGREE offsets within page `pg` using writes */
static inline void hammer_page(volatile unsigned char *region, long pg, unsigned char *wbyte) {
    size_t base = (size_t)pg * PAGE_SIZE;
    for (int s = 0; s < SPATIAL_DEGREE; ++s) {
        size_t offset = (PAGE_SIZE * (size_t)s) / (size_t)SPATIAL_DEGREE;
        write_addr(region + base + offset, (*wbyte)++);
    }
}

/* print progress when hitting each PROGRESS_STEP_PCT boundary */
static inline void maybe_print_progress(long done, long total, int *last_step) {
    int pct = (int)((done * 100) / total);
    int step = pct / PROGRESS_STEP_PCT;
    if (step != *last_step && pct % PROGRESS_STEP_PCT == 0) {
        *last_step = step;
        printf("%d%% ", pct);
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <RSS_in_pages> <total_iterations> <rotations>\n", argv[0]);
        return 1;
    }

    long rss_pages   = atol(argv[1]);
    long total_iters = atol(argv[2]);
    long rotations   = atol(argv[3]);
    if (rss_pages <= 0 || total_iters <= 0 || rotations <= 0) {
        fprintf(stderr, "All arguments must be > 0\n");
        return 1;
    }

    /* split pages: 20%% hot, 80%% cold */
    long hot_pages = (rss_pages * 20) / 100;
    if (hot_pages < 1) hot_pages = 1;
    if (hot_pages > rss_pages) hot_pages = rss_pages;
    long cold_pages = rss_pages - hot_pages;
    long hot_start  = cold_pages;

    /* compute iteration counts */
    long hot_iters  = (total_iters * 80) / 100;          /* 80% of ops go to hot set */
    long cold_iters = total_iters - hot_iters;           /* 20% to cold set */

    /* allocate region (check for overflow) */
    if ((uint64_t)rss_pages > UINT64_MAX / PAGE_SIZE) {
        fprintf(stderr, "Requested region too large\n");
        return 1;
    }
    size_t region_sz = (size_t)rss_pages * PAGE_SIZE;
    volatile unsigned char *region = (volatile unsigned char *)malloc(region_sz);
    if (!region) {
        perror("malloc");
        return 1;
    }

    /* Phase 0: fault in every page with a write */
    unsigned char wbyte = 0xA5;
    for (long p = 0; p < rss_pages; ++p) {
        write_addr(region + (size_t)p * PAGE_SIZE, wbyte++);
    }

    /* prepare per-rotation quotas, distributing remainders */
    long hot_per_rot_base  = hot_iters / rotations;
    long hot_remain        = hot_iters % rotations;
    long cold_per_rot_base = cold_iters / rotations;
    long cold_remain       = cold_iters % rotations;

    long done = 0;
    int last_step = -1;
    printf("Starting write benchmark:\n");

    for (long r = 0; r < rotations; ++r) {
        /* STEP 1: hammer cold pages (writes) */
        long this_cold = cold_per_rot_base + (r < cold_remain ? 1 : 0);
        for (long c = 0; c < this_cold; ++c) {
            long page = cold_pages ? (c % cold_pages) : 0;
            hammer_page(region, page, &wbyte);
            ++done;
            maybe_print_progress(done, total_iters, &last_step);
        }

        /* STEP 2: hammer hot pages (writes) */
        long this_hot = hot_per_rot_base + (r < hot_remain ? 1 : 0);
        for (long h = 0; h < this_hot; ++h) {
            long page = hot_start + (h % hot_pages);
            hammer_page(region, page, &wbyte);
            ++done;
            maybe_print_progress(done, total_iters, &last_step);
        }
    }

    putchar('\n');
    free((void*)region);
    return 0;
}
