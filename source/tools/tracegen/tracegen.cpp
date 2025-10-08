#include "pin.H"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_set>
#include "cache_hierarchy.cpp"  

// ----------------------- Knobs -----------------------
KNOB<UINT64> KnobL1Size   (KNOB_MODE_WRITEONCE, "pintool", "l1size",  "524288",
                           "L1 size (bytes)");
KNOB<UINT32> KnobL1Assoc  (KNOB_MODE_WRITEONCE, "pintool", "l1assoc", "8",
                           "L1 associativity");
KNOB<UINT64> KnobL2Size   (KNOB_MODE_WRITEONCE, "pintool", "l2size",  "4194304",
                           "L2 size (bytes)");
KNOB<UINT32> KnobL2Assoc  (KNOB_MODE_WRITEONCE, "pintool", "l2assoc", "8",
                           "L2 associativity");
KNOB<UINT64> KnobReportIv (KNOB_MODE_WRITEONCE, "pintool", "repival", "1000000000",
                           "Report interval (instructions)");
KNOB<std::string> KnobOut (KNOB_MODE_WRITEONCE, "pintool", "outfile",  "cache_sim.log",
                           "Log output filename");
KNOB<std::string> KnobTrace(KNOB_MODE_WRITEONCE, "pintool", "tracefile", "cache_sim.trace",
                           "Trace output filename");
KNOB<bool>   KnobNoTrace  (KNOB_MODE_WRITEONCE, "pintool", "notrace",  "false",
                           "Disable trace output");
KNOB<UINT64> KnobFF       (KNOB_MODE_WRITEONCE, "pintool", "fftarget", "0",
                           "Fast-forward instruction count");

// ----------------------- Constants -------------------
static constexpr size_t TRACE_REC_SIZE = 1 + sizeof(uint64_t);
static constexpr uint64_t PAGESZ = 4096;

// ----------------------- Globals ---------------------
static std::atomic<uint64_t> gIns{0};
static std::atomic<uint64_t> lastReport{0};
static std::atomic<uint64_t> repIval{0};
static std::atomic<bool>     ffDone{true};
static std::atomic<uint64_t> ffTarget{0};
static std::atomic<bool>     noTrace{false};

static std::atomic<uint64_t> totalMemOps{0};
static std::atomic<uint64_t> totalReads{0};
static std::atomic<uint64_t> totalWrites{0};

static std::atomic<uint64_t> readsTraced{0};
static std::atomic<uint64_t> writesTraced{0};

static std::atomic<uint64_t> l2MissesTot{0};
static std::atomic<uint64_t> wbTot{0};

static std::atomic<uint64_t> l2MissesIv{0};
static std::atomic<uint64_t> wbIv{0};

static std::unordered_set<uint64_t> uniqueMissPages;
static std::atomic<uint64_t> uniqueMissPagesCount{0};

static std::ofstream OutFile;
static std::ofstream TraceFile;

static SimpleCacheInterface* gCache = nullptr;

// ----------------------- Trace I/O -------------------
static inline void WriteTrace(char op, uint64_t addr) {
    if (noTrace.load(std::memory_order_relaxed)) return;
    char rec[TRACE_REC_SIZE];
    rec[0] = op;
    std::memcpy(rec + 1, &addr, sizeof(addr)); // little-endian host
    TraceFile.write(rec, TRACE_REC_SIZE);
}

// ----------------------- Cache call -----------------
static inline void CacheCall(bool isWrite, uint64_t va) {
    // Skip during fast-forward
    if (!ffDone.load(std::memory_order_acquire)) return;

    totalMemOps.fetch_add(1, std::memory_order_relaxed);
    if (isWrite) totalWrites.fetch_add(1, std::memory_order_relaxed);
    else         totalReads .fetch_add(1, std::memory_order_relaxed);

    SimpleCacheInterface::AccessResult result;

    // single lock around cache to keep it simple + thread-safe
    if (isWrite)
    {
        result = gCache->write(va);
    }
    else {
        result = gCache->read(va);
    }

    std::vector<uint64_t> wbs = result.l2_writebacks_to_mem;
    if (!result.l2_hit)
    {
        ++l2MissesIv;
        ++l2MissesTot;
        WriteTrace('r', va);
        if (wbs.size())
        {
            for (auto w : wbs)
            {
                ++wbIv;
                ++wbTot;
                WriteTrace('w', w);
            }
        }
    }
}

// ----------------------- Instrumentation -------------
static VOID CountAndMaybeReport(THREADID /*tid*/) {
    uint64_t cur = gIns.fetch_add(1, std::memory_order_relaxed) + 1;

    // fast-forward
    if (!ffDone.load(std::memory_order_relaxed)) {
        uint64_t ff = ffTarget.load(std::memory_order_relaxed);
        if (ff > 0 && cur >= ff) {
            ffDone.store(true, std::memory_order_release);
        }
    }

    if (!ffDone.load(std::memory_order_acquire)) return;

    uint64_t iv = repIval.load(std::memory_order_relaxed);
    if (iv == 0) return;

    uint64_t expected = lastReport.load(std::memory_order_relaxed);
    if (cur - expected < iv) return;

    if (lastReport.compare_exchange_strong(expected, cur,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
        OutFile << "\n[Interval @ " << cur << " instructions]\n"
                << "L2: miss=" << l2MissesIv.load() << " | Mem WB=" << wbIv.load()
                << " | Unique L2-miss pages=" << uniqueMissPagesCount.load()
                << std::endl;
        l2MissesIv.store(0, std::memory_order_relaxed);
        wbIv.store(0, std::memory_order_relaxed);
    }
}

static VOID RecordRead(VOID* /*ip*/, VOID* addr, THREADID /*tid*/) {
    CacheCall(false, reinterpret_cast<uint64_t>(addr));
}

static VOID RecordWrite(VOID* /*ip*/, VOID* addr, THREADID /*tid*/) {
    CacheCall(true, reinterpret_cast<uint64_t>(addr));
}

static VOID Instruction(INS ins, VOID*) {
    // count every instruction (for FF + interval)
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CountAndMaybeReport,
                   IARG_THREAD_ID, IARG_END);

    if (INS_IsMemoryRead(ins)) {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordRead,
            IARG_INST_PTR, IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);
    }
    if (INS_IsMemoryWrite(ins)) {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordWrite,
            IARG_INST_PTR, IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
    }
}

// ----------------------- Final report ----------------
static VOID Fini(INT32, VOID*) { // Stats gathering

    CacheStats l1stats = gCache->get_l1_stats();
    CacheStats l2stats = gCache->get_l2_stats();

    uint64_t ins  = gIns.load();
    uint64_t r    = totalReads.load();
    uint64_t ac   = l2stats.read_accesses + l2stats.write_accesses;
    uint64_t w    = totalWrites.load();
    uint64_t m    = l2MissesTot.load();
    uint64_t l1a  = l1stats.read_accesses + l1stats.write_accesses;
    uint64_t l1m  = l1a - (l1stats.read_hits + l1stats.write_hits);
    uint64_t wbs  = wbTot.load();
    uint64_t uniq = uniqueMissPagesCount.load();
    uint64_t wt   = writesTraced.load();
    uint64_t rt   = readsTraced.load();

    OutFile << "\n[FINAL REPORT @ " << ins << " instructions]\n"
            << "Memory Ops: " << (r + w) << " (Reads=" << r << ", Writes=" << w << ")\n"
            << "L1: accesses=" << l1a << "\n"
            << "L1: misses=" << l1m << "\n"
            << "L1: missrate=" << ((double)l1m / (double)l1a) * 100 << "\n"
            << "L2: accesses=" << ac << "\n"
            << "L2: misses=" << m << "\n"
            << "L2: missrate=" << ((double)m / (double)ac) * 100 << "\n"
            << "Memory write-backs: " << wbs << "\n"
            << "Unique L2-miss pages: " << uniq << "\n"
            << "Reads traced: " << rt << "\n"
            << "Writes traced: " << wt << "\n"
            << "=========== PROGRAM FINISHED ===========\n";


    OutFile.flush();

    if (!noTrace.load()) {
        TraceFile.flush();
        TraceFile.close();
    }

    delete gCache; // TODO: replace with new remove
    gCache = nullptr;

    OutFile.close();
}

// ----------------------- Main ------------------------
int main(int argc, char* argv[]) {
    if (PIN_Init(argc, argv)) {
        std::cerr << "Pin init failed\n";
        return 1;
    }

    // Files
    OutFile.open(KnobOut.Value());
    if (!OutFile) { std::cerr << "Failed to open " << KnobOut.Value() << "\n"; return 1; }

    noTrace.store(KnobNoTrace.Value(), std::memory_order_relaxed);
    if (!noTrace.load()) {
        TraceFile.open(KnobTrace.Value(), std::ios::binary);
        if (!TraceFile) { std::cerr << "Failed to open " << KnobTrace.Value() << "\n"; return 1; }
    }

    repIval.store(KnobReportIv.Value(), std::memory_order_relaxed);
    ffTarget.store(KnobFF.Value(), std::memory_order_relaxed);
    ffDone.store(KnobFF.Value() == 0, std::memory_order_relaxed);

    // TODO: Replace with cache routine
    gCache = new SimpleCacheInterface(1024, 8, 8192, 8);

    // Banner
    OutFile << "CACHE SIM (simple) PARAMS\n"
            << "Report interval: " << repIval.load() << " ins\n"
            << "Fast-forward: "    << ffTarget.load() << " ins\n"
            << "L1: " << KnobL1Size.Value() << " bytes, " << KnobL1Assoc.Value() << "-way\n"
            << "L2: " << KnobL2Size.Value() << " bytes, " << KnobL2Assoc.Value() << "-way\n"
            << "Trace: " << (noTrace.load() ? "disabled" : "enabled") << "\n"
            << "========================================\n\n";

    // Instrument + go
    INS_AddInstrumentFunction(Instruction, nullptr);
    PIN_AddFiniFunction(Fini, nullptr);
    PIN_StartProgram(); // never returns
    return 0;
}
