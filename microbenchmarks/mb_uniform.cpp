#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>     // for getpid()
#include <string>
#include <unordered_map>
#include <sstream>
#include <iostream>
#include <chrono>

#define PAGE_SIZE 4096

static inline void touch_addr(volatile char *p) {
    char v = *p;
    (void)v;
}

// Parses command-line arguments in flag-value pairs
std::unordered_map<std::string, std::string> parse_args(int argc, char **argv) {
    std::unordered_map<std::string, std::string> args;
    for (int i = 1; i < argc - 1; ++i) {
        std::string key = argv[i];
        if (key[0] == '-' && argv[i + 1][0] != '-') {
            args[key] = argv[i + 1];
            ++i;
        }
    }
    return args;
}

int main(int argc, char **argv) {
    auto args = parse_args(argc, argv);

    auto start = std::chrono::steady_clock::now();

    long rss_pages = args.count("-rss") ? atol(args["-rss"].c_str()) : 100;
    long total_iters = args.count("-total_iters") ? atol(args["-total_iters"].c_str()) : 1000;

    if (rss_pages <= 0 || total_iters <= 0) {
        fprintf(stderr, "Error: -rss and -total_iters must be positive integers.\n");
        return 1;
    }

    // Compute memory split
    long uncompressed_pages = std::max(1L, (rss_pages * 20) / 100);
    long compressed_pages = std::max(1L, rss_pages - uncompressed_pages);

    printf("RSS pages          = %ld\n", rss_pages);
    printf("Uncompressed pages = %ld (20%%)\n", uncompressed_pages);
    printf("Compressed pages   = %ld (80%%)\n", compressed_pages);

    // Allocate and touch memory region
    size_t region_size = (size_t)rss_pages * PAGE_SIZE;
    volatile char *region = (volatile char *)malloc(region_size);
    if (!region) {
        perror("malloc");
        return 1;
    }

    for (long i = 0; i < rss_pages; i++) {
        touch_addr(region + (i * PAGE_SIZE));
    }

    // Sweep all pages in round-robin
    int last_percent = -1;
    bool gcDone = false;
    for (long iter = 0; iter < total_iters; ++iter) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now-start);
        long page = iter % rss_pages;
        touch_addr(region + (page * PAGE_SIZE));

        int percent = ((iter + 1) * 100) / total_iters;
        if (percent % 20 == 0 && percent != last_percent) {
            printf(" [%d%%] ", percent);
            fflush(stdout);
            last_percent = percent;
        }
    }

    free((void *)region);
    return 0;
}

