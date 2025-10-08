// Simple wrapper for cache simulation
// Works with or without CACHE_ENABLE_DATA_CHECKING

#include "cache_hierarchy.cpp"  // Include the main cache implementation

int main() {
    // Note: This example works with or without CACHE_ENABLE_DATA_CHECKING
    // For production simulation, comment out CACHE_ENABLE_DATA_CHECKING
    
    // Create cache: L1 = 32 KiB (128 sets, 4-way)
    //                L2 = 256 KiB (512 sets, 8-way)
    SimpleCacheInterface cache(128, 4, 512, 8);
    
    std::cout << "=== Simple Cache Simulator Example ===\n\n";
    
    // Example 1: Sequential reads
    std::cout << "--- Test 1: Sequential Reads ---\n";
    for (int i = 0; i < 10; ++i) {
        uint64_t addr = i * 64;  // Access different blocks
        auto result = cache.read(addr);
        
        std::cout << "Read 0x" << std::hex << addr << std::dec << ": "
                  << (result.l1_hit ? "L1 HIT" : 
                      result.l2_hit ? "L2 HIT" : "DRAM")
                  << "\n";
    }
    
    // Example 2: Repeated reads (should hit)
    std::cout << "\n--- Test 2: Repeat Same Reads ---\n";
    for (int i = 0; i < 10; ++i) {
        uint64_t addr = i * 64;
        auto result = cache.read(addr);
        
        std::cout << "Read 0x" << std::hex << addr << std::dec << ": "
                  << (result.l1_hit ? "L1 HIT" : 
                      result.l2_hit ? "L2 HIT" : "DRAM")
                  << "\n";
    }
    
    // Example 3: Mixed reads and writes
    std::cout << "\n--- Test 3: Mixed Read/Write ---\n";
    
    // Write to new location
    auto w1 = cache.write(0x1000, 42);
    std::cout << "Write 0x1000: " 
              << (w1.l1_hit ? "L1 HIT" : 
                  w1.l2_hit ? "L2 HIT" : "DRAM") << "\n";
    
    // Read from same location (should hit)
    auto r1 = cache.read(0x1000);
    std::cout << "Read 0x1000: "
              << (r1.l1_hit ? "L1 HIT" : 
                  r1.l2_hit ? "L2 HIT" : "DRAM") << "\n";
    
    // Write again (should hit)
    auto w2 = cache.write(0x1000, 99);
    std::cout << "Write 0x1000: "
              << (w2.l1_hit ? "L1 HIT" : 
                  w2.l2_hit ? "L2 HIT" : "DRAM") << "\n";
    
    // Print final statistics
    std::cout << "\n=== Final Statistics ===\n";
    cache.print_stats();
    
    // Access raw stats
    std::cout << "\n=== Summary ===\n";
    auto l1 = cache.get_l1_stats();
    auto l2 = cache.get_l2_stats();
    
    std::cout << "Total operations: " << (l1.read_accesses + l1.write_accesses) << "\n";
    std::cout << "L1 overall hit rate: " << std::fixed << std::setprecision(2)
              << 100.0 * (l1.read_hits + l1.write_hits) / (l1.read_accesses + l1.write_accesses) 
              << "%\n";
    
    return 0;
}

/* ============================================================================
   INTEGRATION TEMPLATE FOR YOUR TRACE SIMULATOR
   ============================================================================
   
int your_trace_simulator(const char* trace_file) {
    // 1. Create cache with your configuration
    SimpleCacheInterface cache(l1_sets, l1_assoc, l2_sets, l2_assoc);
    
    // 2. Load trace
    std::vector<TraceEntry> trace = load_trace(trace_file);
    
    // 3. Process each access
    uint64_t dram_reads = 0;
    for (auto& entry : trace) {
        SimpleCacheInterface::AccessResult result;
        
        if (entry.is_read) {
            result = cache.read(entry.address);
        } else {
            result = cache.write(entry.address, entry.value);
        }
        
        // Track DRAM accesses
        if (result.dram_read) {
            dram_reads++;
            // Handle DRAM read latency, etc.
        }
        
        // Optional: log hit/miss for this access
        log_access(entry, result);
    }
    
    // 4. Print results
    cache.print_stats();
    std::cout << "Total DRAM reads: " << dram_reads << "\n";
    
    return 0;
}

============================================================================ */