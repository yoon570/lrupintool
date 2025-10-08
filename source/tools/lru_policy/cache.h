#include <cstdint>
#include <iomanip>
#include <tuple>
#include <stdexcept>
#include <iostream>
#include <cassert>

#define LINESZ 64
#define PAGE_SIZE 4096u
static constexpr uint32_t PAGE_SHIFT = 6; // 64 = 1 << 64

static inline uint64_t page_num(uint64_t byte_addr) {
    return byte_addr >> PAGE_SHIFT; // faster and equivalent for power-of-two page size
}



struct Line {
    uint64_t lid = 0, timestamp = 0;
    bool valid = false, dirty = false;
};

std::ostream& operator<<(std::ostream& os, const Line& line) {
    os << "[" << line.lid << ", " << line.timestamp << ", " << line.dirty << "]";
    return os;
}

class SimpleCache {
public:
    explicit SimpleCache(size_t cache_sz, int assoc_)
        : cache_size(cache_sz), assoc(static_cast<uint32_t>(assoc_)),
          accesses(0), misses(0), writebacks(0), cache(nullptr) {
        if (!cache_sz || !assoc_) { perror("Invalid cache size or associativity."); std::exit(1); }

        num_sets = static_cast<uint32_t>(cache_sz / (static_cast<size_t>(assoc_) * LINESZ));
        if (num_sets == 0 || (num_sets & (num_sets - 1)) != 0) {
            perror("Cache sets must be a power of 2 (size must be divisible by assoc*LINESZ).");
            std::exit(1);
        }
        set_mask = num_sets - 1;

        cache = new Line*[num_sets]{};
        for (uint32_t i = 0; i < num_sets; ++i) cache[i] = new Line[assoc]();
    }

    ~SimpleCache() {
        if (cache) {
            for (uint32_t i = 0; i < num_sets; ++i) delete[] cache[i];
            delete[] cache;
        }
    }

    // returns <miss?, wb_addr>
    std::tuple<bool, uint64_t> Access(uint64_t addr, bool is_write) {
        ++accesses;
        const uint64_t line_id = addr / LINESZ;
        const uint32_t set_num = static_cast<uint32_t>(line_id) & set_mask;

        // Hit?
        if (Search(line_id, set_num, is_write)) {
            return {false, 0};
        }

        // Miss path
        ++misses;
        Line* victim = GetFreeSpace(set_num);
        assert(victim != nullptr);

        // Any write-back?
        uint64_t wb_addr = 0;
        if (victim->valid && victim->dirty) {
            ++writebacks;
            wb_addr = victim->lid * LINESZ;
        }

        // Install new line
        victim->lid = line_id;
        victim->valid = true;
        victim->dirty = is_write;     // clean on read, dirty on write
        victim->timestamp = accesses;

        return {true, wb_addr};
    }

    bool make_dirty(uint64_t addr) {
        const uint64_t line_id = addr / LINESZ;
        const uint32_t set_num = static_cast<uint32_t>(line_id) & set_mask;
        for (uint32_t way = 0; way < assoc; ++way) {
            Line& l = cache[set_num][way];
            if (l.valid && l.lid == line_id) { l.dirty = true; return true; }
        }
        return false;
    }

    void debug_print(std::ostream& os) {
        os << "Cachesz: " << cache_size
           << ", num_sets: " << num_sets
           << ", assoc: " << assoc << "\n";
        for (uint32_t i = 0; i < num_sets; ++i) {
            os << i << ":";
            for (uint32_t j = 0; j < assoc; ++j) os << cache[i][j];
            os << "\n";
        }
    }

    uint64_t get_accesses() const { return accesses; }
    uint64_t get_misses() const { return misses; }
    uint64_t get_writebacks() const { return writebacks; }

private:
    bool Search(uint64_t line_id, uint32_t set_num, bool is_write) {
        for (uint32_t way = 0; way < assoc; ++way) {
            Line& l = cache[set_num][way];
            if (l.valid && l.lid == line_id) {
                l.timestamp = accesses;
                if (is_write) l.dirty = true;
                return true;
            }
        }
        return false;
    }

    // First empty slot; else LRU by smallest timestamp.
    Line* GetFreeSpace(uint32_t set_num) {
        for (uint32_t way = 0; way < assoc; ++way)
            if (!cache[set_num][way].valid) return &cache[set_num][way];

        uint32_t victim_way = 0;
        uint64_t best_ts = cache[set_num][0].timestamp;
        for (uint32_t way = 1; way < assoc; ++way) {
            if (cache[set_num][way].timestamp < best_ts) {
                best_ts = cache[set_num][way].timestamp;
                victim_way = way;
            }
        }
        return &cache[set_num][victim_way];
    }

    uint32_t setMask() const { return set_mask; }

    // members
    size_t cache_size;
    uint32_t num_sets = 0, assoc = 0, set_mask = 0;
    uint64_t accesses = 0, misses = 0, writebacks = 0;
    Line** cache;
};

class SimpleCacheConductor {
public:
    explicit SimpleCacheConductor(size_t l1_size, size_t l2_size, int l1_assoc, int l2_assoc) {
        L1 = new SimpleCache(l1_size, l1_assoc);
        L2 = new SimpleCache(l2_size, l2_assoc);
    }
    ~SimpleCacheConductor() {
        delete L1;
        delete L2;
    }

    void debug(std::ostream& os)
    {
        os << "L1\n";
        L1->debug_print(os);
        os << "L2\n";
        L2->debug_print(os);
    }

    uint64_t l2_acc()
    {
        return L2->get_accesses();
    }

    // returns <read_miss_addr, l2_wb_addr>; both 0 means "no trace emit"
    std::tuple<uint64_t, uint64_t> CacheRoutine(uint64_t addr, bool is_write) {
        auto l1 = L1->Access(addr, is_write);
        bool l1_miss = std::get<0>(l1);
        if (!l1_miss) return {0, 0};                 // L1 hit → nothing to trace

        auto l2 = L2->Access(addr, false);           // probe L2 as a read, per spec
        uint64_t l1_wb_addr = std::get<1>(l1);
        if (l1_wb_addr != 0) {
            if (!L2->make_dirty(l1_wb_addr)) {
                perror("L2 line to mark dirty not found after L1 write-back");
                std::exit(1);
            }
        }

        bool l2_miss = std::get<0>(l2);
        uint64_t l2_wb_addr = std::get<1>(l2);
        if (l2_miss) {
            const uint64_t miss_page_1b = page_num(addr) + 1;            // encode 1-based
            const uint64_t wb_page_1b   = l2_wb_addr ? (page_num(l2_wb_addr) + 1) : 0;
            return {miss_page_1b, wb_page_1b};
        }
    }

private:
    SimpleCache* L1 = nullptr;
    SimpleCache* L2 = nullptr;
};
