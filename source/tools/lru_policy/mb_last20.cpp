#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE         4096
#define PROGRESS_STEP_PCT 20          /* print bar every 20 % */

static inline void touch_addr(volatile char *p)
{
    volatile char v = *p;
    (void)v;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <RSS_in_pages> <total_iterations>\n",
                argv[0]);
        return 1;
    }

    long rss_pages   = atol(argv[1]);
    long total_iters = atol(argv[2]);
    if (rss_pages <= 0 || total_iters <= 0) {
        fprintf(stderr, "Both arguments must be > 0\n");
        return 1;
    }

    /* ------------ split pages: 20 % hot, 80 % cold ----------------------- */
    long hot_pages  = (rss_pages * 20) / 100;   /* last 20 % of pages  */
    if (hot_pages < 1)      hot_pages = 1;
    if (hot_pages > rss_pages) hot_pages = rss_pages;
    long cold_pages = rss_pages - hot_pages;    /* first 80 % of pages */
    long hot_start  = cold_pages;               /* first index of hot set */

    /* ------------ allocate ------------------------------------------------ */
    size_t region_sz = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char *)malloc(region_sz);
    if (!region) { perror("malloc"); return 1; }

    /* Phase 0: fault-in all pages once */
    for (long i = 0; i < rss_pages; ++i)
        touch_addr(region + i * PAGE_SIZE);

    /* Phase 1: 80 % iterations → hot-set, 20 % → cold-set */
    long hot_iters  = (total_iters * 80) / 100;
    long cold_iters = total_iters - hot_iters;

    int last_pct = -1;

    /* STEP 1 */
    /* ---- first 80 %: hammer last 20 % of pages -------------------------- */
    for (long i = 0; i < hot_iters; ++i) {
        long page = hot_start + (i % hot_pages);         /* wrap inside hot set */
        touch_addr(region + page * PAGE_SIZE);

        int pct = (int)(((i + 1) * 100) / total_iters);
        if (pct / PROGRESS_STEP_PCT != last_pct &&
            pct % PROGRESS_STEP_PCT == 0) {
            last_pct = pct / PROGRESS_STEP_PCT;
            printf("%d%% ", pct); fflush(stdout);
        }
    }

    /* STEP 2 */
    /* ---- final 20 %: touch first 80 % of pages -------------------------- */
    for (long i = 0; i < cold_iters; ++i) {
        long page = (i % cold_pages);                      /* wrap inside cold set */
        touch_addr(region + page * PAGE_SIZE);

        int pct = (int)(((hot_iters + i + 1) * 100) / total_iters);
        if (pct / PROGRESS_STEP_PCT != last_pct &&
            pct % PROGRESS_STEP_PCT == 0) {
            last_pct = pct / PROGRESS_STEP_PCT;
            printf("%d%% ", pct); fflush(stdout);
        }
    }

    putchar('\n');
    free((void *)region);
    return 0;
}
