// cache_hierarchy.cpp (single file)
//
// Build options (pick one main OR none):
//   -DCACHE_ENABLE_DATA_CHECKING -DCACHE_BUILD_TEST_MAIN   // full tests with data verification
//   -DCACHE_BUILD_SIMPLE_MAIN                              // simple interface demo
//
// If embedding into your simulator, compile without either *_MAIN flag and include this TU.

#include <cstdint>
#include <iostream>
#include <vector>
#include <cassert>
#include <random>
#include <iomanip>
#include <cstring>
#include <memory>

static constexpr size_t BLOCK_SIZE = 64;

// ============================================================================
// Address Helpers
// ============================================================================
inline uint64_t get_block_addr(uint64_t byte_addr) { return (byte_addr / BLOCK_SIZE) * BLOCK_SIZE; }
inline uint64_t get_block_offset(uint64_t byte_addr) { return byte_addr % BLOCK_SIZE; }
inline uint64_t get_block_id(uint64_t byte_addr) { return byte_addr / BLOCK_SIZE; }

// ============================================================================
// Statistics
// ============================================================================
struct CacheStats {
    uint64_t read_hits = 0;
    uint64_t read_accesses = 0;
    uint64_t write_hits = 0;
    uint64_t write_accesses = 0;

    void print(const std::string& name) const {
        double read_rate  = read_accesses  ? double(read_hits)  / read_accesses  : 0.0;
        double write_rate = write_accesses ? double(write_hits) / write_accesses : 0.0;
        std::cout << name << ":\n";
        std::cout << "  Reads: " << read_accesses << ", Writes: " << write_accesses << "\n";
        std::cout << "  Read hitrate: "  << std::fixed << std::setprecision(3) << read_rate  * 100 << "%\n";
        std::cout << "  Write hitrate: " << std::fixed << std::setprecision(3) << write_rate * 100 << "%\n";
    }
};

// ============================================================================
// Lower level interface
// ============================================================================
class LowerLevel {
public:
    virtual ~LowerLevel() = default;
    virtual std::vector<uint8_t> read_block(uint64_t address, bool for_write = false) = 0;
    virtual void write_block(const std::vector<uint8_t>& block, uint64_t address) = 0;
};

// ============================================================================
// Cache Line
// ============================================================================
struct CacheLine {
    std::vector<uint8_t> data;  // empty when invalid
    uint64_t age = 0;
    uint64_t start_addr = 0;
    bool dirty = false;
    uint32_t index = 0;
    uint64_t tag = 0;
    bool valid() const { return !data.empty(); }
};

// ============================================================================
// Cache (UNCHANGED LOGIC)
// ============================================================================
class Cache : public LowerLevel {
private:
    uint32_t sets;
    uint32_t associativity;
    std::vector<std::vector<CacheLine>> cache;
    uint64_t global_age;
    CacheStats stats;

public:
    LowerLevel* lower_cache;
    Cache* higher_cache;

    Cache(uint32_t sets, uint32_t associativity)
        : sets(sets), associativity(associativity), global_age(0),
          lower_cache(nullptr), higher_cache(nullptr) {
        cache.resize(sets);
        for (auto& set : cache) set.resize(associativity);
    }

    struct OIT { uint64_t offset, index, tag; };
    OIT find_offset_index_tag(uint64_t address) {
        uint64_t block_id = address / BLOCK_SIZE;
        uint64_t offset = address % BLOCK_SIZE;
        uint64_t tag = block_id / sets;
        uint32_t index = block_id % sets;
        return {offset, index, tag};
    }

    // read_block returns a copy of the block
    std::vector<uint8_t> read_block(uint64_t address, bool for_write = false) override {
        if (!for_write) stats.read_accesses++;

        auto oit = find_offset_index_tag(address);
        address -= oit.offset; // align to block

        auto& set = cache[oit.index];

        // hit
        for (auto& line : set) {
            if (line.valid() && line.tag == oit.tag) {
                if (!for_write) stats.read_hits++;
                line.age = global_age++;
                return line.data; // copy
            }
        }

        // miss
        if (for_write) {
            // preserve original accounting quirk
            stats.write_hits--; // compensate for increment in write_block
        }

        auto data = lower_cache->read_block(address, for_write);
        insert(address, data, false);
        return data;
    }

    void write_block(const std::vector<uint8_t>& block, uint64_t address) override {
        stats.write_accesses++;

        auto oit = find_offset_index_tag(address);
        auto& set = cache[oit.index];

        // hit
        for (auto& line : set) {
            if (line.valid() && line.tag == oit.tag) {
                stats.write_hits++;
                line.age = global_age++;
                line.data = block; // copy
                line.dirty = true;
                return;
            }
        }

        // miss: insert (can happen after back-invalidation)
        insert(address, block, true);
    }

    void insert(uint64_t address, const std::vector<uint8_t>& block, bool dirty) {
        auto oit = find_offset_index_tag(address);
        auto& set = cache[oit.index];

        bool all_full = true;
        for (auto& line : set) if (!line.valid()) { all_full = false; break; }
        if (all_full) evict_oldest(oit.index);

        for (auto& line : set) {
            if (!line.valid()) {
                line.data = block;
                line.age = global_age++;
                line.start_addr = address;
                line.dirty = dirty;
                line.index = oit.index;
                line.tag = oit.tag;
                return;
            }
        }
    }

    void evict_block(uint64_t address) {
        auto oit = find_offset_index_tag(address);
        auto& set = cache[oit.index];

        for (auto& line : set) {
            if (line.valid() && line.tag == oit.tag) {
                if (line.dirty) lower_cache->write_block(line.data, line.start_addr);
                line.dirty = false;
                line.data.clear(); // invalidate
            }
        }

        // back-invalidate
        if (higher_cache) higher_cache->evict_block(address);
    }

    void evict_oldest(uint32_t set_index) {
        auto& set = cache[set_index];
        size_t oldest_idx = 0;
        uint64_t oldest_age = set[0].age;
        for (size_t i = 1; i < set.size(); ++i) if (set[i].age < oldest_age) { oldest_age = set[i].age; oldest_idx = i; }

        auto& oldest = set[oldest_idx];
        if (oldest.dirty) lower_cache->write_block(oldest.data, oldest.start_addr);
        evict_block(oldest.start_addr);
    }

    const CacheStats& get_stats() const { return stats; }
};

// ============================================================================
// DRAM backends (always exposed)
// ============================================================================
#ifdef CACHE_ENABLE_DATA_CHECKING
// Real DRAM with byte-accurate contents
class DRAMMemory : public LowerLevel {
private:
    std::vector<uint8_t> memory;
    uint64_t read_accesses = 0, write_accesses = 0;
public:
    explicit DRAMMemory(size_t size) : memory(size, 0) {}
    std::vector<uint8_t> read_block(uint64_t block_start_addr, bool /*for_write*/ = false) override {
        read_accesses++;
        std::vector<uint8_t> block(BLOCK_SIZE);
        assert(block_start_addr + BLOCK_SIZE <= memory.size());
        std::memcpy(block.data(), &memory[block_start_addr], BLOCK_SIZE);
        return block;
    }
    void write_block(const std::vector<uint8_t>& block, uint64_t block_start_addr) override {
        write_accesses++;
        assert(block_start_addr + BLOCK_SIZE <= memory.size());
        std::memcpy(&memory[block_start_addr], block.data(), BLOCK_SIZE);
    }
    uint8_t read_byte(uint64_t addr) const { return memory[addr]; }
    void write_byte(uint64_t addr, uint8_t value) { memory[addr] = value; }
    void print_stats() const {
        std::cout << "DRAM reads: " << read_accesses << ", DRAM writes: " << write_accesses << "\n";
    }
};
#else
// Dummy DRAM: counts traffic, returns zeroed blocks, discards writes
class DummyMemory : public LowerLevel {
private:
    uint64_t read_accesses = 0, write_accesses = 0;
public:
    explicit DummyMemory(size_t /*unused_size*/ = 0) {}
    std::vector<uint8_t> read_block(uint64_t /*addr*/, bool /*for_write*/ = false) override {
        read_accesses++;
        return std::vector<uint8_t>(BLOCK_SIZE, 0);
    }
    void write_block(const std::vector<uint8_t>& /*block*/, uint64_t /*addr*/) override {
        write_accesses++;
    }
    void print_stats() const {
        std::cout << "DRAM reads: " << read_accesses << ", DRAM writes: " << write_accesses << " (dummy)\n";
    }
};
#endif

// ============================================================================
// Writeback Sniffer (records addresses sent via write_block)
// ============================================================================
class WritebackSniffer : public LowerLevel {
public:
    explicit WritebackSniffer(LowerLevel* target) : target_(target) {}

    std::vector<uint8_t> read_block(uint64_t addr, bool for_write = false) override {
        return target_->read_block(addr, for_write);
    }

    void write_block(const std::vector<uint8_t>& block, uint64_t addr) override {
        writebacks_.push_back(addr);          // block-aligned address
        target_->write_block(block, addr);    // forward
    }

    // Take-and-clear for per-access reporting
    std::vector<uint64_t> take_and_clear() {
        std::vector<uint64_t> out;
        out.swap(writebacks_);
        return out;
    }

private:
    LowerLevel* target_;
    std::vector<uint64_t> writebacks_;
};

// ============================================================================
// MemoryView (always exposed; backend selected by typedef)
// ============================================================================
template <typename MemT>
class MemoryViewT {
private:
    Cache L1;
    Cache L2;
    MemT  mem;

    // Sniffers are allocated after L2/mem exist (avoid init-order pitfalls)
    std::unique_ptr<WritebackSniffer> l1_to_l2_sniffer;
    std::unique_ptr<WritebackSniffer> l2_to_mem_sniffer;

public:
    MemoryViewT(uint32_t l1_sets, uint32_t l1_assoc,
                uint32_t l2_sets, uint32_t l2_assoc, size_t mem_size)
        : L1(l1_sets, l1_assoc), L2(l2_sets, l2_assoc), mem(mem_size)
    {
        // Hook up hierarchy with sniffers in the data path
        l1_to_l2_sniffer = std::make_unique<WritebackSniffer>(&L2);
        l2_to_mem_sniffer = std::make_unique<WritebackSniffer>(&mem);

        L1.lower_cache   = l1_to_l2_sniffer.get();
        L2.higher_cache  = &L1;
        L2.lower_cache   = l2_to_mem_sniffer.get();
    }

    // Instruction format: [addr, 0] for read; [addr, 1, value] for write
    int execute_rw(const std::vector<uint64_t>& instruction) {
        if (instruction.size() == 2) {
            uint64_t addr = instruction[0];
            auto oit = L1.find_offset_index_tag(addr);
            auto block = L1.read_block(addr);
            return block[oit.offset];
        } else {
            uint64_t addr = instruction[0];
            uint8_t value = static_cast<uint8_t>(instruction[2]);
            auto oit = L1.find_offset_index_tag(addr);
            auto block = L1.read_block(addr, true);  // read-allocate path
            block[oit.offset] = value;
            L1.write_block(block, addr);
            return -1;
        }
    }

    void report() const {
        L1.get_stats().print("L1");
        L2.get_stats().print("L2");
        mem.print_stats();
    }

    // Expose and clear per-access writeback logs
    std::vector<uint64_t> take_l1_writebacks() { return l1_to_l2_sniffer->take_and_clear(); }
    std::vector<uint64_t> take_l2_writebacks() { return l2_to_mem_sniffer->take_and_clear(); }

    const Cache& get_l1() const { return L1; }
    const Cache& get_l2() const { return L2; }
};

#ifdef CACHE_ENABLE_DATA_CHECKING
using MemoryView = MemoryViewT<DRAMMemory>;
#else
using MemoryView = MemoryViewT<DummyMemory>;
#endif

// ============================================================================
// Simple plug-in interface (works with or without data checking)
// ============================================================================
class SimpleCacheInterface {
private:
    MemoryView cache;
    CacheStats prev_l1{}, prev_l2{};

    void update_prev_stats() {
        prev_l1 = cache.get_l1().get_stats();
        prev_l2 = cache.get_l2().get_stats();
    }

public:
    struct AccessResult {
        bool l1_hit;
        bool l2_hit;            // only meaningful if l1_hit == false
        bool dram_read;         // true if fetch came from DRAM
        // NEW: writeback addresses produced by this access (block-aligned)
        std::vector<uint64_t> l1_writebacks_to_l2;   // dirty evictions from L1
        std::vector<uint64_t> l2_writebacks_to_mem;  // dirty evictions from L2
    };

    // mem_size_bytes is only used when data checking is enabled
    SimpleCacheInterface(uint32_t l1_sets, uint32_t l1_assoc,
                         uint32_t l2_sets, uint32_t l2_assoc,
                         size_t mem_size_bytes = 0)
        : cache(l1_sets, l1_assoc, l2_sets, l2_assoc, mem_size_bytes) {
        update_prev_stats();
    }

    AccessResult read(uint64_t address) {
        std::vector<uint64_t> instr = {address, 0};
        cache.execute_rw(instr);

        auto curr_l1 = cache.get_l1().get_stats();
        auto curr_l2 = cache.get_l2().get_stats();

        AccessResult r{};
        r.l1_hit = (curr_l1.read_hits > prev_l1.read_hits);
        if (!r.l1_hit) {
            r.l2_hit    = (curr_l2.read_hits > prev_l2.read_hits);
            r.dram_read = !r.l2_hit;
        } else {
            r.l2_hit = false;
            r.dram_read = false;
        }

        // collect writebacks triggered by this access
        r.l1_writebacks_to_l2 = cache.take_l1_writebacks();
        r.l2_writebacks_to_mem = cache.take_l2_writebacks();

        update_prev_stats();
        return r;
    }

    AccessResult write(uint64_t address, uint8_t value = 0) {
        std::vector<uint64_t> instr = {address, 1, value};
        cache.execute_rw(instr);

        auto curr_l1 = cache.get_l1().get_stats();
        auto curr_l2 = cache.get_l2().get_stats();

        AccessResult r{};
        r.l1_hit = (curr_l1.write_hits > prev_l1.write_hits);
        if (!r.l1_hit) {
            // write miss does read-allocate; check L2 read hit
            r.l2_hit    = (curr_l2.read_hits > prev_l2.read_hits);
            r.dram_read = !r.l2_hit;
        } else {
            r.l2_hit = false;
            r.dram_read = false;
        }

        // collect writebacks triggered by this access
        r.l1_writebacks_to_l2 = cache.take_l1_writebacks();
        r.l2_writebacks_to_mem = cache.take_l2_writebacks();

        update_prev_stats();
        return r;
    }

    void print_stats() { cache.report(); }
    const CacheStats& get_l1_stats() { return cache.get_l1().get_stats(); }
    const CacheStats& get_l2_stats() { return cache.get_l2().get_stats(); }
};

// ============================================================================
// Test harness (only when both data checking + this main are enabled)
// ============================================================================
#if defined(CACHE_ENABLE_DATA_CHECKING) && defined(CACHE_BUILD_TEST_MAIN)
struct MemoryAccess { uint64_t addr; bool is_write; uint8_t write_value; };

static std::vector<MemoryAccess> generate_traffic(
        size_t count, uint64_t addr_low, uint64_t addr_high, double write_ratio) {
    std::vector<MemoryAccess> accesses;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> addr_dist(addr_low, addr_high);
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);
    std::bernoulli_distribution write_dist(write_ratio);
    for (size_t i = 0; i < count; ++i) {
        accesses.push_back({addr_dist(rng), write_dist(rng), static_cast<uint8_t>(byte_dist(rng))});
    }
    return accesses;
}

class Baseline {
private:
    std::vector<uint8_t> memory;
public:
    explicit Baseline(size_t size) : memory(size, 0) {}
    int execute_rw(const MemoryAccess& acc) {
        if (acc.is_write) { memory[acc.addr] = acc.write_value; return -1; }
        return memory[acc.addr];
    }
};

static bool run_test(uint32_t l1_sets, uint32_t l1_assoc, uint32_t l2_sets, uint32_t l2_assoc,
                     size_t mem_size, size_t num_accesses, double write_ratio) {
    std::cout << "\n=== Test: L1 " << l1_sets << "×" << l1_assoc
              << ", L2 " << l2_sets << "×" << l2_assoc << " ===\n";

    MemoryView mv(l1_sets, l1_assoc, l2_sets, l2_assoc, mem_size);
    Baseline baseline(mem_size);

    auto accesses = generate_traffic(num_accesses, 0, mem_size - 1, write_ratio);

    size_t mismatches = 0;
    for (size_t i = 0; i < accesses.size(); ++i) {
        const auto& acc = accesses[i];
        std::vector<uint64_t> instr = acc.is_write ? std::vector<uint64_t>{acc.addr, 1, acc.write_value}
                                                   : std::vector<uint64_t>{acc.addr, 0};
        int mv_result = mv.execute_rw(instr);
        int bl_result = baseline.execute_rw(acc);

        if (mv_result != bl_result) {
            std::cout << "MISMATCH at " << i << ": addr=0x" << std::hex << acc.addr << std::dec
                      << " " << (acc.is_write ? "WRITE" : "READ")
                      << " cache=" << mv_result << " baseline=" << bl_result << "\n";
            if (++mismatches >= 10) break;
        }
    }

    if (mismatches == 0) {
        std::cout << "✓ All " << num_accesses << " accesses matched!\n";
        mv.report();
        return true;
    }
    std::cout << "✗ " << mismatches << " mismatches\n";
    return false;
}

int main() {
    std::cout << "=== Cache Simulator - Data Verification Mode ===\n";
    bool t1 = run_test(16, 2, 32, 4, 16*1024,   10000, 0.30);
    bool t2 = run_test(64, 4, 64, 8, 64*1024,   20000, 0.30);
    bool t3 = run_test(64, 8, 128,16, 256*1024, 50000, 0.25);
    std::cout << "\nSummary: " << (t1 && t2 && t3 ? "PASS" : "FAIL") << "\n";
    return (t1 && t2 && t3) ? 0 : 1;
}
#endif

// ============================================================================
// Simple interface example main (works with or without data checking)
// ============================================================================
#ifdef CACHE_BUILD_SIMPLE_MAIN
int main() {
    // L1: 32 KiB (128 sets × 4-way), L2: 256 KiB (512 sets × 8-way)
#ifdef CACHE_ENABLE_DATA_CHECKING
    // provide real DRAM size when checking data
    SimpleCacheInterface cache(128, 4, 512, 8, 1 << 20);
#else
    // size ignored by dummy memory
    SimpleCacheInterface cache(128, 4, 512, 8);
#endif

    std::cout << "=== Simple Cache Simulator Example ===\n\n";

    std::cout << "--- Test 1: Sequential Reads ---\n";
    for (int i = 0; i < 10; ++i) {
        uint64_t addr = static_cast<uint64_t>(i) * BLOCK_SIZE;
        auto res = cache.read(addr);
        std::cout << "Read 0x" << std::hex << addr << std::dec << ": "
                  << (res.l1_hit ? "L1 HIT" : res.l2_hit ? "L2 HIT" : "DRAM");
        if (!res.l1_writebacks_to_l2.empty() || !res.l2_writebacks_to_mem.empty()) {
            std::cout << " | WBs:";
            for (auto a : res.l1_writebacks_to_l2) std::cout << " L1->L2:0x" << std::hex << a << std::dec;
            for (auto a : res.l2_writebacks_to_mem) std::cout << " L2->MEM:0x" << std::hex << a << std::dec;
        }
        std::cout << "\n";
    }

    std::cout << "\n--- Test 2: Repeat Same Reads ---\n";
    for (int i = 0; i < 10; ++i) {
        uint64_t addr = static_cast<uint64_t>(i) * BLOCK_SIZE;
        auto res = cache.read(addr);
        std::cout << "Read 0x" << std::hex << addr << std::dec << ": "
                  << (res.l1_hit ? "L1 HIT" : res.l2_hit ? "L2 HIT" : "DRAM");
        if (!res.l1_writebacks_to_l2.empty() || !res.l2_writebacks_to_mem.empty()) {
            std::cout << " | WBs:";
            for (auto a : res.l1_writebacks_to_l2) std::cout << " L1->L2:0x" << std::hex << a << std::dec;
            for (auto a : res.l2_writebacks_to_mem) std::cout << " L2->MEM:0x" << std::hex << a << std::dec;
        }
        std::cout << "\n";
    }

    std::cout << "\n--- Test 3: Mixed Read/Write ---\n";
    auto w1 = cache.write(0x1000, 42);
    std::cout << "Write 0x1000: " << (w1.l1_hit ? "L1 HIT" : w1.l2_hit ? "L2 HIT" : "DRAM");
    if (!w1.l1_writebacks_to_l2.empty() || !w1.l2_writebacks_to_mem.empty()) {
        std::cout << " | WBs:";
        for (auto a : w1.l1_writebacks_to_l2) std::cout << " L1->L2:0x" << std::hex << a << std::dec;
        for (auto a : w1.l2_writebacks_to_mem) std::cout << " L2->MEM:0x" << std::hex << a << std::dec;
    }
    std::cout << "\n";

    auto r1 = cache.read(0x1000);
    std::cout << "Read 0x1000: "  << (r1.l1_hit ? "L1 HIT" : r1.l2_hit ? "L2 HIT" : "DRAM") << "\n";

    auto w2 = cache.write(0x1000, 99);
    std::cout << "Write 0x1000: " << (w2.l1_hit ? "L1 HIT" : w2.l2_hit ? "L2 HIT" : "DRAM");
    if (!w2.l1_writebacks_to_l2.empty() || !w2.l2_writebacks_to_mem.empty()) {
        std::cout << " | WBs:";
        for (auto a : w2.l1_writebacks_to_l2) std::cout << " L1->L2:0x" << std::hex << a << std::dec;
        for (auto a : w2.l2_writebacks_to_mem) std::cout << " L2->MEM:0x" << std::hex << a << std::dec;
    }
    std::cout << "\n";

    std::cout << "\n=== Final Statistics ===\n";
    cache.print_stats();

    auto l1 = cache.get_l1_stats();
    std::cout << "\n=== Summary ===\n";
    std::cout << "Total operations: " << (l1.read_accesses + l1.write_accesses) << "\n";
    std::cout << "L1 overall hit rate: " << std::fixed << std::setprecision(2)
              << 100.0 * (l1.read_hits + l1.write_hits) / std::max<uint64_t>(1, (l1.read_accesses + l1.write_accesses))
              << "%\n";
    return 0;
}
#endif