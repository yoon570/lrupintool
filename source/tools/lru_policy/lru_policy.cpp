#include "pin.H"
#include <unordered_set>
#include <type_traits>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <limits>
#include <string>
#include <cstring>
#include <ctime>
#include <sstream>
#include "hashll.h"

using namespace HASHLL;

// -----------------------------------------------------------------------
// Knobs for Pintool, parameter sweep
// -----------------------------------------------------------------------
KNOB<UINT64> KnobL1Size   	(KNOB_MODE_WRITEONCE, "pintool", "l1size",  "32768", // match the L1 cache size in the TMCC paper for actual experimental runs
                            "L1 size (bytes)"); // Size of L2 in the processor, private cache hits which allows for parallelization
KNOB<UINT32> KnobL1Assoc  	(KNOB_MODE_WRITEONCE, "pintool", "l1assoc", "8",
                            "L1 associativity");
KNOB<UINT64> KnobL2Size   	(KNOB_MODE_WRITEONCE, "pintool", "l2size",  "262144", // match the L3 cache size in the TMCC paper for actual experimental runs
                            "L2 size (bytes)");
KNOB<UINT32> KnobL2Assoc  	(KNOB_MODE_WRITEONCE, "pintool", "l2assoc", "8",
                            "L2 associativity");
KNOB<UINT32> KnobBlkBytes 	(KNOB_MODE_WRITEONCE, "pintool", "blk",     "64",
                            "Cache-line size");
KNOB<UINT32> KnobUncompressedListSize 
						   	(KNOB_MODE_WRITEONCE, "pintool", "unclsize","262144",
							"Size of uncompressed page LRU list");
KNOB<UINT32> KnobCompressedListSize 
						   	(KNOB_MODE_WRITEONCE, "pintool", "clsize",  "262144",
							"Size of compressed page LRU list");
KNOB<UINT32> KnobPromoteUncompressedFrequency
						   	(KNOB_MODE_WRITEONCE, "pintool", "unclfreq","65536" ,
							"Promotion frequency of uncompressed LRU list");
KNOB<UINT32> KnobPromoteCompressedFrequency
						   	(KNOB_MODE_WRITEONCE, "pintool", "clfreq",  "65536" ,
							"Promotion frequency of compressed LRU list");
KNOB<UINT32> KnobExpansionFrequency
							(KNOB_MODE_WRITEONCE, "pintool", "exfreq",  "65536" ,
							"Expansion frequency for promoting compressed page to uncompressed");
KNOB<UINT64> KnobReportInterval
							(KNOB_MODE_WRITEONCE, "pintool", "repival",  "100000000" , //100 mil by default
							"Report interval in # of instructions");
KNOB<std::string> KnobOutputFilename(KNOB_MODE_WRITEONCE, "pintool", "outfile", "", "Log output filename");
KNOB<std::string> KnobTraceFilename(KNOB_MODE_WRITEONCE, "pintool", "tracefile", "", "Trace output filename");
KNOB<UINT64> KnobFastForward(KNOB_MODE_WRITEONCE, "pintool", "fftarget", "0", "Skip instrumentation for this many instructions");
KNOB<bool> KnobTraceEnable(KNOB_MODE_WRITEONCE, "pintool", "traceenable", "false", "Enable trace?");

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
enum : UINT32 { READ_OP = 0, WRITE_OP = 1 };
enum { access_data=0};

#define CACHELINE_OFFSET           0
#define DATA_BLOCK_FLOOR_ADDR_MASK ~(static_cast<UINT64>(KnobBlkBytes.Value()-1))
#define PAGE_SIZE				   4096


static std::atomic<uint64_t> globalIns{0};
static std::atomic<uint64_t> lastReportIns{0};
static std::atomic<uint64_t> expansionFrequency{0};
static std::atomic<uint64_t> uc_epoch{0};   // since last unclist *mutation*
static std::atomic<uint64_t> cl_epoch{0};   // since last clist  *mutation*
static std::atomic<uint64_t> ex_epoch{0};

static std::atomic<uint64_t> L1AccTot   {0}, L1MissTot   {0};
static std::atomic<uint64_t> L2AccTot   {0}, L2MissTot   {0};
static std::atomic<uint64_t> ClistTot   {0}, UnclistTot  {0}, CpageTot {0};
static std::atomic<uint64_t> ExpansionTot {0}, PromotionTot {0}, HotlistTot {0};
static std::atomic<uint64_t> ExpansionCount {0}, PromotionCount {0}, HotlistCount {0};
static std::atomic<uint64_t> FFTarget {0};
static std::atomic<bool> FFDone {false};
static std::atomic<bool> TraceEnable {false};

static std::atomic<uint64_t> UniqueL2MissPagesCount{0};   // NEWSTATS 
static std::unordered_set<UINT64> uniqueL2MissPages;                         

HashLL * clist = nullptr;
HashLL * unclist = nullptr;


// -----------------------------------------------------------------------
// Cache structures/simulation
// -----------------------------------------------------------------------
struct SimpleCacheConfig {
    uint64_t sizeBytes;
    uint32_t blockBytes;
    uint32_t ways;
    uint32_t sets()      const { return static_cast<uint32_t>(sizeBytes /
                             (blockBytes * ways)); }
    uint32_t blockLog2() const { return 63 - __builtin_clzll(blockBytes); }
    uint32_t setBits()   const { return 63 - __builtin_clzll(sets());    }
};

struct Line { uint64_t tag=0; uint32_t age=0; bool valid=false, dirty=false; };

inline void TouchLRU(std::vector<Line>& w, uint32_t hit)
{ uint32_t a=w[hit].age; for(auto& l:w) if(l.valid&&l.age<a) ++l.age; w[hit].age=0; }

inline uint32_t PickVictim(const std::vector<Line>& w)
{
    uint32_t idx=0,maxAge=0;
    for(uint32_t i=0;i<w.size();++i)
        if(!w[i].valid) return i;
        else if(w[i].age>=maxAge){ idx=i; maxAge=w[i].age; }
    return idx;
}

class SimpleCache
{
public:
    explicit SimpleCache(const SimpleCacheConfig& c)
        : cfg(c), mask(cfg.sets()-1),
          sets(cfg.sets(), std::vector<Line>(cfg.ways)) {}

    template<typename Upper, typename WB>
    bool Access(uint64_t addr, bool isWrite, Upper up, WB wb)
    {
        ++acc;
        auto [set,tag] = Decode(addr);
        auto& w = sets[set];

        // lookup
        for(uint32_t i=0;i<w.size();++i)
            if(w[i].valid && w[i].tag == tag){
                TouchLRU(w,i);
                if(isWrite) w[i].dirty = true;
                return true;                     // hit
            }

        ++miss;
        uint32_t v = PickVictim(w); Line ev = w[v];

        // handle eviction
        if(ev.valid){
            if constexpr(!std::is_same_v<Upper,std::nullptr_t>)
                up(Reconstruct(set, ev.tag), ev.dirty);
            if constexpr(!std::is_same_v<WB,std::nullptr_t>)
                if(ev.dirty) wb(Reconstruct(set, ev.tag));
        }

        for(auto& l : w) if(l.valid) ++l.age;    // age others
        w[v] = { tag, 0, true, isWrite };
        return false;                            // miss
    }

    void Install(uint64_t addr, bool dirty)
    {
        auto [set,tag] = Decode(addr);
        auto& w = sets[set];
        uint32_t v = PickVictim(w); Line ev = w[v];

        if(ev.valid && ev.dirty && wbInstall)
            wbInstall(Reconstruct(set, ev.tag));

        for(auto& l : w) if(l.valid) ++l.age;
        w[v] = { tag, 0, true, dirty };
    }

    void SetWBInstall(std::function<void(uint64_t)> f) { wbInstall = std::move(f); }
    uint64_t Accesses() const { return acc; }
    uint64_t Misses()   const { return miss; }
	void ResetStats()	{ acc = 0; miss = 0; }

private:
    std::pair<uint32_t,uint64_t> Decode(uint64_t a) const
    {
        uint64_t blk = a >> cfg.blockLog2();
        return { static_cast<uint32_t>(blk & mask), blk >> cfg.setBits() };
    }
    uint64_t Reconstruct(uint32_t s, uint64_t tag) const
    { return ((tag << cfg.setBits()) | s) << cfg.blockLog2(); }

    SimpleCacheConfig cfg;
    uint32_t mask;
    std::vector<std::vector<Line>> sets;
    uint64_t acc = 0, miss = 0;
    std::function<void(uint64_t)> wbInstall;
};

// -----------------------------------------------------------------------
// Global state vars
// -----------------------------------------------------------------------

// Output file
static std::ofstream OutFile;
static std::ofstream TraceFile;

// Pin and cache simulation
PIN_LOCK              l2Lock;
PIN_LOCK			  reset_lock;
PIN_LOCK			  unc_lock;
PIN_LOCK   			  c_lock;
PIN_LOCK              cpage_lock;
PIN_LOCK              unique_pages_lock;     
PIN_LOCK 			  trace_lock;
SimpleCacheConfig     cfgL1, cfgL2;
SimpleCache*          L2 = nullptr;          // created in main()
std::vector<SimpleCache*> L1;                // per thread

// LRU list access counters and frequency vars
std::atomic<uint64_t> clist_access   {0};
std::atomic<uint64_t> unclist_access {0};
std::atomic<uint64_t> cpage_access   {0};

std::atomic<uint64_t> clist_freq 	= 0;
std::atomic<uint64_t> unclist_freq	= 0;
std::atomic<uint64_t> report_interval= 0;

struct StatPack { 
	std::atomic<uint64_t> ins=0;
	std::atomic<uint64_t> memIns=0;
	std::atomic<uint64_t> reads=0;
	std::atomic<uint64_t> writes=0; 
};
std::vector<std::unique_ptr<StatPack>> stats;

// -----------------------------------------------------------------------
// Helper methods and prototypes
// -----------------------------------------------------------------------
VOID CacheCall(THREADID, UINT32, UINT64, UINT64, UINT64, UINT32, bool, int, UINT64);
VOID RecordMemRead (VOID*, VOID*, UINT32, ADDRINT, ADDRINT, THREADID);
VOID RecordMemWrite(VOID*, VOID*, UINT32, ADDRINT, ADDRINT, THREADID);

static PIN_LOCK init_lock;

static void EnsureThreadData(THREADID tid)
{
    // fast path – only if tid is already in-bounds
    if (tid < stats.size() && stats[tid] && L1[tid])
        return;

    PIN_GetLock(&init_lock, 0);
    if (tid >= stats.size()) {                 // make room if needed
        stats.resize(tid + 1);
        L1.resize(tid + 1, nullptr);
    }
    if (!L1[tid])      L1[tid]    = new SimpleCache(cfgL1);
    if (!stats[tid])   stats[tid] = std::make_unique<StatPack>();
    PIN_ReleaseLock(&init_lock);
}

// 9-byte record: [1 byte op]['r' or 'w'] + [8 bytes addr little-endian]
inline void TraceWrite(char op, uint64_t addr, THREADID tid) {
    char rec[1 + sizeof(uint64_t)];
    rec[0] = op;
    std::memcpy(rec + 1, &addr, sizeof(addr));  // raw 8 bytes (LE on x86-64)

    PIN_GetLock(&trace_lock, tid + 1);
    TraceFile.write(rec, sizeof(rec));
    PIN_ReleaseLock(&trace_lock);
}


// -----------------------------------------------------------------------
// CacheCall cache access routine
// -----------------------------------------------------------------------
VOID CacheCall(THREADID tid, UINT32 op, UINT64 /*icount*/, UINT64 /*pc*/,
               UINT64 blkAddr, UINT32 /*stk*/, bool /*isPT*/, int /*accType*/, UINT64 vp_addr)
{
    SimpleCache& l1 = *L1[tid];

	// L1 hit
    if(l1.Access(blkAddr, op==WRITE_OP, nullptr, nullptr))
	{
		return;
	}

    bool l2Hit;
    {
        PIN_GetLock(&l2Lock, 0);
        l2Hit = L2->Access(blkAddr, op==WRITE_OP,
                 /*install in L1*/ [&](uint64_t a,bool d){ l1.Install(a,d); },
                 /*mem write-back*/ [](uint64_t /*a*/){});
        PIN_ReleaseLock(&l2Lock);
    }

    if(!l2Hit){
	/*	
		Procedure:
		Check for promotions in compressed->uncompressed
		Check if node is in either list
		If uncompressed LRU is not full, add a node
		If compressed LRU is not full, add a node
		If its a hit on a compressed list page, and access threshold is hit for unclist
			Promotion of clist page is needed, evict from unclist and add new page
		If it is in neither, it is a compressed page *outside* LRU
		Eviction is needed for the compressed list, use knob for clist for frequency
 	*/		
		PIN_GetLock(&unique_pages_lock, 0);                      // unique misses
		if (uniqueL2MissPages.insert(vp_addr / 4096).second) {
			++UniqueL2MissPagesCount;
		}
		PIN_ReleaseLock(&unique_pages_lock);


		if (ex_epoch.load(std::memory_order_relaxed)
    		>= expansionFrequency.load(std::memory_order_relaxed)) {
			PIN_GetLock(&unc_lock, tid+1);
			PIN_GetLock(&c_lock,  tid+1);
			bool ex_occurred = clist->drop_cold_and_expand(*unclist);       // promotion

			if (ex_occurred) 
			{
				++ExpansionCount; // TODO: Statistics issue, this should be guarded, if expansion does not occur this should not be incremented.
				ex_epoch.store(0);// both lists mutated
			}
			PIN_ReleaseLock(&c_lock);
			PIN_ReleaseLock(&unc_lock);
		}

		PIN_GetLock(&unc_lock, tid+1);
		bool inUnc = (unclist->find_node(vp_addr) != nullptr);
		bool unclFull = unclist->isFull();
		PIN_ReleaseLock(&unc_lock);

		PIN_GetLock(&c_lock, tid+1);
		bool inCl  = (clist ->find_node(vp_addr) != nullptr);
		bool clFull = clist->isFull();
		PIN_ReleaseLock(&c_lock);

		// 1) Try to fill unclist
		if (!inUnc && !unclFull) {
			PIN_GetLock(&unc_lock, tid+1);
			unclist->touch(vp_addr);
			++uc_epoch;
			PIN_ReleaseLock(&unc_lock);
		}
		else if (!inCl && !clFull && clist_freq.load() <= cl_epoch.load()) { // This appears to be the problematic line here.
			// 2) Try to fill clist (only if unclist branch did NOT run)
			PIN_GetLock(&c_lock, tid+1);
			clist->touch(vp_addr);
			PIN_ReleaseLock(&c_lock);
		}

		/*  Step 3 : page is already in unclist */
		PIN_GetLock(&unc_lock, tid+1);
		auto victim = unclist->find_node(vp_addr);
		if (victim) {
			++unclist_access;
			++uc_epoch;
			if (uc_epoch >= unclist_freq) {
				unclist->touch(vp_addr);      // refresh order
				++PromotionCount;
				uc_epoch = 0;
			} else {
				unclist->increment_count(vp_addr);
			}
			PIN_ReleaseLock(&unc_lock);
			return;
		}
		PIN_ReleaseLock(&unc_lock);
		++cl_epoch;
		++ex_epoch;

		/*  Step 4 : page is already in clist, or try to insert/refresh there */
		PIN_GetLock(&c_lock, tid+1);
		victim = clist->find_node(vp_addr);
		if (victim) {
			++clist_access;
			clist->touch(vp_addr);        // refresh / move to MRU
			PIN_ReleaseLock(&c_lock);
			return;
		} // add update requirement here (LRU updates)
		else if (cl_epoch >= clist_freq) {         // insert new page, evicting LRU
			++HotlistCount;
			clist->touch(vp_addr);
			++cpage_access;
			cl_epoch = 0;
			PIN_ReleaseLock(&c_lock);
			return;
		}
		PIN_ReleaseLock(&c_lock);

		/*  Step 5 : none of the above –– count as compressed-page miss */
		PIN_GetLock(&cpage_lock, tid+1);
		++cpage_access;
		PIN_ReleaseLock(&cpage_lock);
    }
}

// -----------------------------------------------------------------------
// Recording memory reads/writes
// -----------------------------------------------------------------------
VOID RecordMemRead(VOID* ip, VOID* addr, UINT32 stk,
                   ADDRINT rbp, ADDRINT rsp, THREADID tid)
{
	if (!FFDone.load(std::memory_order_acquire)) {
		return; // skip trace and simulation entirely during fast-forward
	}

	if (TraceEnable) {
		uint64_t lineAddr = (((UINT64)addr + CACHELINE_OFFSET) & DATA_BLOCK_FLOOR_ADDR_MASK);
		TraceWrite('r', lineAddr, tid+1);
	}

	EnsureThreadData(tid);
    (void)ip; (void)rbp; (void)rsp; (void)stk;
    stats[tid]->memIns.fetch_add(1, std::memory_order_relaxed);
	stats[tid]->reads.fetch_add(1, std::memory_order_relaxed);
	UINT64 vp_addr = (UINT64)addr;
    CacheCall(tid, READ_OP, 0, (UINT64)ip,
              ((UINT64)addr + CACHELINE_OFFSET) & DATA_BLOCK_FLOOR_ADDR_MASK,
              stk, false, access_data, vp_addr);
}

VOID RecordMemWrite(VOID* ip, VOID* addr, UINT32 stk,
                    ADDRINT rbp, ADDRINT rsp, THREADID tid)
{
	if (!FFDone.load(std::memory_order_acquire)) {
		return; // skip trace and simulation entirely during fast-forward
	}

	if (TraceEnable) {
		uint64_t lineAddr = (((UINT64)addr + CACHELINE_OFFSET) & DATA_BLOCK_FLOOR_ADDR_MASK);
		TraceWrite('w', lineAddr, tid+1);
	}

	EnsureThreadData(tid);
    (void)ip; (void)rbp; (void)rsp; (void)stk;
    stats[tid]->memIns.fetch_add(1, std::memory_order_relaxed);
	stats[tid]->writes.fetch_add(1, std::memory_order_relaxed);
	UINT64 vp_addr = (UINT64)addr;
    CacheCall(tid, WRITE_OP, 0, (UINT64)ip,
              ((UINT64)addr + CACHELINE_OFFSET) & DATA_BLOCK_FLOOR_ADDR_MASK,
              stk, false, access_data, vp_addr);
}

void WriteFinalReport()
{
    // Snapshot local stats
    uint64_t l1Acc = 0, l1Miss = 0;
    for (auto* c : L1) if (c) { l1Acc += c->Accesses(); l1Miss += c->Misses(); }

    uint64_t l2Acc  = L2 ? L2->Accesses() : 0;
    uint64_t l2Miss = L2 ? L2->Misses()   : 0;

    uint64_t intervalUnc  = unclist_access.load(std::memory_order_relaxed);
    uint64_t intervalCpg  = cpage_access.load(std::memory_order_relaxed);
	uint64_t intervalClist= clist_access.load(std::memory_order_relaxed);

    // Reset per-interval counters
    unclist_access.store(0, std::memory_order_relaxed);
    cpage_access  .store(0, std::memory_order_relaxed);
	clist_access  .store(0, std::memory_order_relaxed);

    // Accumulate grand totals
    L1AccTot   += l1Acc;
    L1MissTot  += l1Miss;
    L2AccTot   += l2Acc;
    L2MissTot  += l2Miss;
    UnclistTot += intervalUnc;
    CpageTot   += intervalCpg;
	ClistTot   += intervalClist;
	ExpansionTot += ExpansionCount;
	PromotionTot += PromotionCount;
	HotlistTot += HotlistCount;

	// -------- print report --------
	OutFile << "\n[Report @ " << globalIns << " instructions]\n"
			<< "L1 accesses: " << l1Acc
			<< ", misses: "     << l1Miss
			<< ", L2 accesses : " << l2Acc
			<< ", misses: "     << l2Miss
			<< "\nClist Accesses: " << intervalClist << " ("
			<< std::fixed << std::setprecision(5) 
			<< ((double) intervalClist / (double)L2->Misses()) * 100.0 << "%)"
			<< ", Unclist Accesses: " << intervalUnc << " ("
			<< std::fixed << std::setprecision(5)
			<< ((double) intervalUnc/ (double)L2->Misses()) * 100.0 << "%)"
			<< ", Cpage Accesses: " << intervalCpg << " ("
			<< std::fixed << std::setprecision(5)
			<< ((double) intervalCpg / (double)L2->Misses()) * 100.0 << "%)";
	OutFile << "\nExpansionCount: " << ExpansionCount << "\n"
			<< "PromotionCount: " << PromotionCount << "\n"
			<< "HotlistCount: " << HotlistCount << "\n";

	OutFile << "\n[FINAL TOTALS]\n"
			<< "L1 accesses: " << L1AccTot
			<< ", misses: "     << L1MissTot
			<< ", L2 accesses : " << L2AccTot
			<< ", misses: "     << L2MissTot
			<< "\nClist Accesses: " << ClistTot << " ("
			<< std::fixed << std::setprecision(5) 
			<< ((double) ClistTot / L2MissTot) * 100.0 << "%)"
			<< ", Unclist Accesses: " << UnclistTot << " ("
			<< std::fixed << std::setprecision(5)
			<< ((double) UnclistTot / L2MissTot) * 100.0 << "%)"
			<< ", Cpage Accesses: " << CpageTot << " ("
			<< std::fixed << std::setprecision(5)
			<< ((double) CpageTot / L2MissTot) * 100.0 << "%)";
	OutFile << "\nExpansionCount: " << ExpansionTot << "\n"
			<< "PromotionCount: " << PromotionTot << "\n"
			<< "HotlistCount: " << HotlistTot << "\n"
			<< "Unique L2-miss pages: "
          	<< UniqueL2MissPagesCount.load() << "\n"; 
	
	OutFile << std::dec << "\n=========== PROGRAM FINISHED ============\n";

    // Clean up
    delete L2;
    delete unclist;
}


// -----------------------------------------------------------------------
// Instrumentation functions
// -----------------------------------------------------------------------
VOID Instruction(INS ins, VOID*)
{
    UINT32 stkStatus = 0;                 // could refine with REG_RSP vs REG_RBP

    if(INS_IsMemoryRead(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead,
            IARG_INST_PTR, IARG_MEMORYREAD_EA, IARG_UINT32, stkStatus,
            IARG_REG_VALUE, REG_RBP, IARG_REG_VALUE, REG_RSP,
            IARG_THREAD_ID, IARG_END);

    if(INS_IsMemoryWrite(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite,
            IARG_INST_PTR, IARG_MEMORYWRITE_EA, IARG_UINT32, stkStatus,
            IARG_REG_VALUE, REG_RBP, IARG_REG_VALUE, REG_RSP,
            IARG_THREAD_ID, IARG_END);

    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)+[](THREADID tid){

		uint64_t cur = ++globalIns;                        // total instructions
		if (!FFDone.load(std::memory_order_relaxed) &&
			FFTarget.load(std::memory_order_relaxed) &&
			cur >= FFTarget.load(std::memory_order_relaxed)) {
			FFDone.store(true, std::memory_order_release);
		}

		EnsureThreadData(tid);
   		stats[tid]->ins.fetch_add(1, std::memory_order_relaxed);

		if (!FFDone.load(std::memory_order_acquire)) return;

		uint64_t expected = lastReportIns.load(std::memory_order_relaxed);
		if ((cur - expected) > report_interval)
		{
			// let **one** thread do the report
			if (lastReportIns.compare_exchange_strong(expected, cur))
			{
				// -------- aggregate L1 --------
				PIN_GetLock(&reset_lock, tid+1);
				uint64_t l1Acc = 0, l1Miss = 0;
				for (auto* c : L1) {
					if (c) { l1Acc += c->Accesses(); l1Miss += c->Misses(); }
				}


				// -------- aggregate L2 --------
				uint64_t l2Acc  = L2 ? L2->Accesses() : 0;
				uint64_t l2Miss = L2 ? L2->Misses()   : 0;

				// ----- accumulate grand-totals -----
				L1AccTot   += l1Acc;
				L1MissTot  += l1Miss;
				L2AccTot   += l2Acc;
				L2MissTot  += l2Miss;

				ClistTot   += clist_access.load();
				UnclistTot += unclist_access.load();
				CpageTot   += cpage_access.load();

				ExpansionTot += ExpansionCount;
				PromotionTot += PromotionCount;
				HotlistTot   += HotlistCount;


				// -------- print report --------
				OutFile << "\n[Report @ " << cur << " instructions]\n"
						<< "L1 accesses: " << l1Acc
						<< ", misses: "     << l1Miss
						<< ", L2 accesses : " << l2Acc
						<< ", misses: "     << l2Miss
						<< "\nClist Accesses: " << clist_access << " ("
						<< std::fixed << std::setprecision(5) 
        				<< ((double)clist_access / (double)L2->Misses()) * 100.0 << "%)"
						<< ", Unclist Accesses: " << unclist_access << " ("
						<< std::fixed << std::setprecision(5)
        				<< ((double)unclist_access / (double)L2->Misses()) * 100.0 << "%)"
						<< ", Cpage Accesses: " << cpage_access << " ("
						<< std::fixed << std::setprecision(5)
        				<< ((double)cpage_access / (double)L2->Misses()) * 100.0 << "%)";
				OutFile << "\nExpansionCount: " << ExpansionCount << "\n"
						<< "PromotionCount: " << PromotionCount << "\n"
						<< "HotlistCount: " << HotlistCount << "\n";
						

				// Statistics reset occurs here:
				for (auto * c : L1)
				{
					if (c)
					{
						c->ResetStats();
					}
				}

				if (L2)
				{
					L2->ResetStats();
				}
				PIN_GetLock(&unc_lock, 0);			
				unclist->reset_counters();
				PIN_ReleaseLock(&unc_lock);
				PIN_GetLock(&c_lock, 0);
				clist->reset_counters();
				PIN_ReleaseLock(&c_lock);
				clist_access	= 0;
				unclist_access	= 0;
				cpage_access	= 0;
				ExpansionCount  = 0;
				PromotionCount  = 0;
				HotlistCount    = 0;
				for (auto& sptr : stats) {
					if (sptr) {
						sptr->ins   .store(0, std::memory_order_relaxed);
						sptr->memIns.store(0, std::memory_order_relaxed);
						sptr->reads .store(0, std::memory_order_relaxed);
						sptr->writes.store(0, std::memory_order_relaxed);
					}
				}

				PIN_ReleaseLock(&reset_lock);
			}
		}
	}, IARG_THREAD_ID, IARG_END);
}

// -----------------------------------------------------------------------
// Thread spinup and destruction
// -----------------------------------------------------------------------
VOID ThreadStart(THREADID tid, CONTEXT*, INT32, VOID*)
{
	PIN_GetLock(&init_lock, 0);
    if (tid >= L1.size()) {
        L1.resize(tid+1, nullptr);
        stats.resize(tid+1);
    }
    L1[tid] = new SimpleCache(cfgL1);
    
    if (!stats[tid]) {
        stats[tid] = std::make_unique<StatPack>();
	}
	PIN_ReleaseLock(&init_lock);
}



VOID ThreadFini(THREADID tid, const CONTEXT*, INT32, VOID*)
{
    //delete L1[tid];
	//L1[tid] = nullptr;
}

// -----------------------------------------------------------------------
// Report print
// -----------------------------------------------------------------------
VOID Fini(INT32, VOID*)
{
    WriteFinalReport();
}

// -----------------------------------------------------------------------
// Main method, execution + params here
// -----------------------------------------------------------------------
int main(int argc, char* argv[])
{

	if(PIN_Init(argc, argv)){
        std::cerr << "Pin init failed\n";
        return 1;
    }

	// Setting up knobs
	uint32_t unclsize 	  = KnobUncompressedListSize.Value();
	uint32_t clsize   	  = KnobCompressedListSize.Value();
	unclist_freq		  = KnobPromoteUncompressedFrequency.Value();
	clist_freq	  		  = KnobPromoteCompressedFrequency.Value();
	report_interval       = KnobReportInterval.Value();
	expansionFrequency = KnobExpansionFrequency.Value();

	std::string outFilename = KnobOutputFilename.Value();  
	std::string traceFilename = KnobTraceFilename.Value();

	TraceEnable = KnobTraceEnable.Value();

	FFTarget = KnobFastForward.Value();

	const uint64_t ff = KnobFastForward.Value();        // absent on CLI -> uses default 0
	FFTarget.store(ff, std::memory_order_relaxed);
	// Optional means: 0 -> no fast-forward, start immediately
	FFDone.store(ff == 0, std::memory_order_relaxed);

	
	OutFile.open(outFilename);
	if (!OutFile)
	{
		if (outFilename.length() < 1)
			outFilename = "NO FILENAME SPECIFIED";
		std::cerr << "FAILED TO OPEN LOG FILE: " << outFilename << std::endl;
		return 1;
	}

	if (TraceEnable) {
		TraceFile.open(traceFilename, std::ios::binary);
		if (!TraceFile)
		{
			if (traceFilename.length() < 1)
				traceFilename = "NO FILENAME SPECIFIED";
			std::cerr << "FAILED TO OPEN TRACE FILE: " << traceFilename << std::endl;
			return 1;
		}
	}

	// Initializing page doubly linked lists
	clist   = new HASHLL::HashLL(clsize);
	unclist = new HASHLL::HashLL(unclsize);

	/* 
		Clist and unclist sizes are parameters... we need to measure RSS for those.
		Is there a means of determining the RSS of the program that is being run
		THROUGH Pin?	--> Issue resolved, this is done through the unique pages measurement.
	*/

    cfgL1 = { KnobL1Size.Value(), KnobBlkBytes.Value(), KnobL1Assoc.Value() };
    cfgL2 = { KnobL2Size.Value(), KnobBlkBytes.Value(), KnobL2Assoc.Value() };
    L2    = new SimpleCache(cfgL2);

	OutFile << "LRUPOLICY STARTING WITH PARAMETERS:\n"
		<< "UNCLSIZE: " << unclsize
		<< "\nCLSIZE: " << clsize
		<< "\nUNCLFREQ: " << unclist_freq
		<< "\nEXFREQ: " << expansionFrequency
		<< "\nCLFREQ: " << clist_freq
		<< "\nREPORTIVAL: " << report_interval
		<< "\nCACHELINE SIZE: " << KnobBlkBytes.Value()
		<< "\nTRACEENABLE: " << KnobTraceEnable.Value()
		<< "\nL1Size: " << KnobL1Size.Value()
		<< "\nL1Assoc: " << KnobL1Assoc.Value()
		<< "\nL2Size: " << KnobL2Size.Value()
		<< "\nL2Assoc: " << KnobL2Assoc.Value()
		<< "\n";

    PIN_InitLock(&l2Lock);
	PIN_InitLock(&reset_lock);
	PIN_InitLock(&unc_lock);
	PIN_InitLock(&c_lock);
	PIN_InitLock(&cpage_lock);
	PIN_InitLock(&init_lock);
	PIN_InitLock(&trace_lock);
	PIN_InitLock(&unique_pages_lock);                  

    INS_AddInstrumentFunction(Instruction,  nullptr);
    PIN_AddThreadStartFunction(ThreadStart, nullptr);
    PIN_AddThreadFiniFunction (ThreadFini,  nullptr);
    PIN_AddFiniFunction       (Fini,        nullptr);       

    PIN_StartProgram();    // never returns
    return 0;
}
