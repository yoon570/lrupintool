#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE 4096

static inline void touch_addr(volatile char *p) {
    char v = *p;  // read only
    (void)v;      // suppress unused variable warning
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <RSS_in_pages> <total_iterations>\n", argv[0]);
        return 1;
    }

    long rss_pages   = atol(argv[1]);
    long total_iters = atol(argv[2]);
    if (rss_pages <= 0 || total_iters <= 0) {
        fprintf(stderr, "Both RSS and total_iterations must be > 0\n");
        return 1;
    }

    // (1) Compute the 20% uncompressed, 80% compressed split:
    long uncompressed_pages = (rss_pages * 20) / 100;
    if (uncompressed_pages < 1)       uncompressed_pages = 1;
    if (uncompressed_pages > rss_pages) uncompressed_pages = rss_pages;

    long compressed_pages = rss_pages - uncompressed_pages;
    if (compressed_pages < 1) compressed_pages = 1;

    printf("RSS pages          = %ld\n", rss_pages);
    printf("Uncompressed pages = %ld (20%%)\n", uncompressed_pages);
    printf("Compressed pages   = %ld (80%%)\n", compressed_pages);

    // (2) Allocate memory for the working region.
    size_t region_size = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char*)malloc(region_size);
    if (!region) {
        perror("malloc");
        return 1;
    }

    // (3) Touch every page once to ensure they are mapped
    for (long i = 0; i < rss_pages; i++) {
        touch_addr((volatile char *)(region + (i * PAGE_SIZE)));
    }

    // (4) Evenly sweep *all* pages, total_iters touches in round-robin
    int last_printed_percent = -1;

    for (long iter = 0; iter < total_iters; ++iter) {
        long page = iter % rss_pages;
        touch_addr((volatile char *)(region + (page * PAGE_SIZE)));

        int percent_done = (int)(((iter + 1) * 100) / total_iters);
        if (percent_done % 20 == 0 && percent_done != last_printed_percent) {
            printf("%d%% ", percent_done);
            fflush(stdout); // optional, ensures immediate printing
            last_printed_percent = percent_done;
        }
    }

    
    printf("Done.\n");

    free((void*)region);
    return 0;
}
