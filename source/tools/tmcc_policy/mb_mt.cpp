// ---------------------------------------------------------------------------
// microbench_omp.cpp  –  Multithreaded (OpenMP) version
//   Compile:  g++ -O2 -fopenmp -std=c++17 microbench_omp.cpp -o microbench_omp
//   Run:      OMP_NUM_THREADS=<N> ./microbench_omp <RSS_pages> <total_iters>
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <omp.h>

#define PAGE_SIZE 4096

static inline void touch_addr(volatile char* p) {
    char v = *p;           // read-only “touch”
    (void)v;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <RSS_in_pages> <total_iterations>\n",
                     argv[0]);
        return 1;
    }

    const long rss_pages   = std::atol(argv[1]);
    const long total_iters = std::atol(argv[2]);
    if (rss_pages <= 0 || total_iters <= 0) {
        std::fprintf(stderr,
                     "Both RSS_in_pages and total_iterations must be > 0\n");
        return 1;
    }

    // (1) 20 % “uncompressed” / 80 % “compressed” bookkeeping
    long uncompressed_pages = (rss_pages * 20) / 100;
    if (uncompressed_pages < 1)         uncompressed_pages = 1;
    if (uncompressed_pages > rss_pages) uncompressed_pages = rss_pages;
    const long compressed_pages = rss_pages - uncompressed_pages;

    std::printf("RSS pages          = %ld\n", rss_pages);
    std::printf("Uncompressed pages = %ld (20%%)\n", uncompressed_pages);
    std::printf("Compressed pages   = %ld (80%%)\n", compressed_pages);

    // (2) Allocate the working region
    const size_t region_size = static_cast<size_t>(rss_pages) * PAGE_SIZE;
    volatile char* region = static_cast<volatile char*>(std::malloc(region_size));
    if (!region) {
        perror("malloc");
        return 1;
    }

    // (3) Fault-in every page once (parallel is okay here too)
    #pragma omp parallel for schedule(static)
    for (long i = 0; i < rss_pages; ++i)
        touch_addr(region + i * PAGE_SIZE);

    // (4) Parallel round-robin sweep: total_iters touches in all
    int last_printed_percent = -1;   // shared among threads

    #pragma omp parallel for schedule(static)
    for (long iter = 0; iter < total_iters; ++iter) {
        long page = iter % rss_pages;
        touch_addr(region + page * PAGE_SIZE);

        // Lightweight progress indicator every 20 %
        int percent_done = static_cast<int>((iter + 1) * 100 / total_iters);
        if (percent_done % 20 == 0) {
            #pragma omp critical
            {
                if (percent_done != last_printed_percent) {
                    std::printf("%d%% ", percent_done);
                    std::fflush(stdout);
                    last_printed_percent = percent_done;
                }
            }
        }
    }

    std::printf("Done.\n");
    std::free(const_cast<char*>(reinterpret_cast<char const volatile*>(region)));
    return 0;
}
