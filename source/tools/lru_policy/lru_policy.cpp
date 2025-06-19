// ============================================================================
// pintool_cache.cpp  –  LRU cache simulator + basic stats collection
// ============================================================================
#include "pin.H"
#include <type_traits>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <limits>
#include "hashll.h"

using namespace HASHLL;

// -----------------------------------------------------------------------
// Knobs for Pintool, parameter sweep
// -----------------------------------------------------------------------
KNOB<UINT64> KnobL1Size   	(KNOB_MODE_WRITEONCE, "pintool", "l1size",  "32768",
                            "L1 size (bytes)");
KNOB<UINT32> KnobL1Assoc  	(KNOB_MODE_WRITEONCE, "pintool", "l1assoc", "8",
                            "L1 associativity");
KNOB<UINT64> KnobL2Size   	(KNOB_MODE_WRITEONCE, "pintool", "l2size",  "262144",
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
KNOB<char* const> KnobOutfile
							(KNOB_MODE_WRITEONCE, "pintool", "outfile",  "fini.out" ,
							"Output location");

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
enum : UINT32 { READ_OP = 0, WRITE_OP = 1 };
enum { access_data=0, access_inst=1, access_page_table=2 };

#define CACHELINE_OFFSET           0
#define DATA_BLOCK_FLOOR_ADDR_MASK ~(static_cast<UINT64>(KnobBlkBytes.Value()-1))
#define PAGE_SIZE				   4096

const uint64_t MAXVAL = std::numeric_limits<uint64_t>::max();

constexpr uint64_t REPORT_INTERVAL = 1'000'000'000ULL;  // e.g. 1 billion
constexpr uint64_t MAX_INTERVAL = MAXVAL;
static std::atomic<uint64_t> globalIns{0};
static std::atomic<uint64_t> lastReportIns{0};
static std::atomic<uint64_t> expansionFrequency{0};
static std::atomic<uint64_t> uc_epoch{0};   // since last unclist *mutation*
static std::atomic<uint64_t> cl_epoch{0};   // since last clist  *mutation*

std::ofstream Out; // output file

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

// Pin and cache simulation
PIN_LOCK              l2Lock;
PIN_LOCK			  reset_lock;
PIN_LOCK			  unc_lock;
PIN_LOCK   			  c_lock;
PIN_LOCK              cpage_lock;
SimpleCacheConfig     cfgL1, cfgL2;
SimpleCache*          L2 = nullptr;          // created in main()
std::vector<SimpleCache*> L1;                // per thread

// LRU list access counters and frequency vars
uint64_t clist_access   = 0;
uint64_t unclist_access = 0;
uint64_t cpage_access	= 0;

uint64_t clist_freq 	= 0;
uint64_t unclist_freq	= 0;

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

		PIN_GetLock(&unc_lock, tid+1);
		if (!unclist->isFull()) {
			unclist->touch(vp_addr);          // insert as MRU
			++unclist_access;
			PIN_ReleaseLock(&unc_lock);
			return;
		}
		PIN_ReleaseLock(&unc_lock);

		/*  Step 2 : insert into clist if it still has room */
		PIN_GetLock(&c_lock, tid+1);
		if (!clist->isFull()) {
			clist->touch(vp_addr);            // insert / move to MRU
			++clist_access;
			PIN_ReleaseLock(&c_lock);
			return;
		}
		PIN_ReleaseLock(&c_lock);

		/*  Step 0 : lists are full –– do we swap? (promotion) */
		PIN_GetLock(&unc_lock, tid+1);
		PIN_GetLock(&c_lock,  tid+1);
		if (uc_epoch >= expansionFrequency) {
			clist->swap_with(*unclist);       // promotion
			uc_epoch = 0;                     // both lists mutated
		}
		PIN_ReleaseLock(&c_lock);
		PIN_ReleaseLock(&unc_lock);

		/*  Step 3 : page is already in unclist */
		PIN_GetLock(&unc_lock, tid+1);
		auto victim = unclist->find_node(vp_addr);
		if (victim) {
			++unclist_access;
			if (uc_epoch >= unclist_freq) {
				unclist->touch(vp_addr);      // refresh order
				uc_epoch = 0;
			} else {
				unclist->increment_count(vp_addr);
			}
			PIN_ReleaseLock(&unc_lock);
			return;
		}
		PIN_ReleaseLock(&unc_lock);

		/*  Step 4 : page is already in clist, or try to insert/refresh there */
		PIN_GetLock(&c_lock, tid+1);
		victim = clist->find_node(vp_addr);
		if (victim) {
			++clist_access;
			if (cl_epoch >= clist_freq) {
				clist->touch(vp_addr);        // refresh / move to MRU
				cl_epoch = 0;
			} else {
				clist->increment_count(vp_addr);
			}
			PIN_ReleaseLock(&c_lock);
			return;
		}

		if (cl_epoch >= clist_freq) {         // insert new page, evicting LRU
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
	++uc_epoch;
	++cl_epoch;

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
	++uc_epoch;
	++cl_epoch;

    (void)ip; (void)rbp; (void)rsp; (void)stk;
    stats[tid]->memIns.fetch_add(1, std::memory_order_relaxed);
	stats[tid]->writes.fetch_add(1, std::memory_order_relaxed);
	UINT64 vp_addr = (UINT64)addr;
    CacheCall(tid, WRITE_OP, 0, (UINT64)ip,
              ((UINT64)addr + CACHELINE_OFFSET) & DATA_BLOCK_FLOOR_ADDR_MASK,
              stk, false, access_data, vp_addr);
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
    stats[tid]->ins.fetch_add(1, std::memory_order_relaxed);
		uint64_t cur = ++globalIns;                        // total instructions
		if ((cur - lastReportIns.load(std::memory_order_relaxed)) > MAX_INTERVAL)
		{
			// let **one** thread do the report
			if (lastReportIns.compare_exchange_strong(cur, cur))
			{
				/* 
					Maybe locking is needed here? At the very least check
					In any case, this is where we will also flush out all
					counters for both clist and unclist accesses.
					LRU order is preserved, we just want to record and 
					prevent an integer overflow. 
					I guess flushing is not needed if we just use a uint64.
					Maybe flush just in case? Not sure we'll ever hit that
					cap but it wouldn't hurt to be sure. God knows how many
					instructions are present in a seven-to-ten hour benchmark.
				 */
				// -------- aggregate L1 --------
				uint64_t l1Acc = 0, l1Miss = 0;
				for (auto* c : L1) {
					if (c) { l1Acc += c->Accesses(); l1Miss += c->Misses(); }
				}

				// -------- aggregate L2 --------
				uint64_t l2Acc  = L2 ? L2->Accesses() : 0;
				uint64_t l2Miss = L2 ? L2->Misses()   : 0;

				// -------- print report --------
				Out << "\n[Report @ " << cur << " instructions]\n"
						<< "  L1 accesses : " << l1Acc
						<< "\n  misses: "     << l1Miss
						<< "\n  MPKI: "       << std::fixed << std::setprecision(2)
						<< (cur ? 1000.0 * l1Miss / cur : 0.0) << '\n'
						<< "  L2 accesses : " << l2Acc
						<< "\n  misses: "     << l2Miss
						<< "\n  MPKI: "       << std::fixed << std::setprecision(2)
						<< (cur ? 1000.0 * l2Miss / cur : 0.0) << "\n"
						<< "\n  Clist Accesses: " << clist_access
						<< "\n  Unclist Accesses: " << unclist_access
						<< "\n  Cpage   Accesses: " << cpage_access;

				// Statistics reset occurs here:
				PIN_GetLock(&reset_lock, tid+1);
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
							
				unclist->reset_counters();
				clist->reset_counters();
				clist_access	= 0;
				unclist_access	= 0;
				cpage_access	= 0;
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
		else if ((cur - lastReportIns.load(std::memory_order_relaxed)) > REPORT_INTERVAL)
		{
			// let **one** thread do the report
			if (lastReportIns.compare_exchange_strong(cur, cur))
			{
				// -------- aggregate L1 --------
				uint64_t l1Acc = 0, l1Miss = 0;
				for (auto* c : L1) {
					if (c) { l1Acc += c->Accesses(); l1Miss += c->Misses(); }
				}

				// -------- aggregate L2 --------
				uint64_t l2Acc  = L2 ? L2->Accesses() : 0;
				uint64_t l2Miss = L2 ? L2->Misses()   : 0;

				// -------- print report --------
				Out << "\n[Report @ " << cur << " instructions]\n"
						<< "  L1 accesses : " << l1Acc
						<< "\n  misses: "     << l1Miss
						<< "\n  MPKI: "       << std::fixed << std::setprecision(2)
						<< (cur ? 1000.0 * l1Miss / cur : 0.0) << '\n'
						<< "  L2 accesses : " << l2Acc
						<< "\n  misses: "     << l2Miss
						<< "\n  MPKI: "       << std::fixed << std::setprecision(2)
						<< (cur ? 1000.0 * l2Miss / cur : 0.0) << "\n"
						<< "\n  Clist Accesses: " << clist_access
						<< "\n  Unclist Accesses: " << unclist_access
						<< "\n  Cpage   Accesses: " << cpage_access;
			}
		}
	}, IARG_THREAD_ID, IARG_END);
}

// -----------------------------------------------------------------------
// Thread spinup and destruction
// -----------------------------------------------------------------------
VOID ThreadStart(THREADID tid, CONTEXT*, INT32, VOID*)
{
    if (tid >= L1.size()) {
        L1.resize(tid+1, nullptr);
        stats.resize(tid+1);  // now this makes each stats[tid] == nullptr
    }
    L1[tid] = new SimpleCache(cfgL1);

    // allocate a new StatPack for this thread
    stats[tid] = std::make_unique<StatPack>();
}


VOID ThreadFini(THREADID tid, const CONTEXT*, INT32, VOID*)
{
    delete L1[tid];
}

// -----------------------------------------------------------------------
// Report print
// -----------------------------------------------------------------------
VOID Fini(INT32, VOID*)
{
    uint64_t totIns=0, totMem=0, rd=0, wr=0;
    for(auto& s:stats)
	{ 
		if (s)
		{
			totIns	+= s->ins	.load(std::memory_order_relaxed);
			totMem	+= s->memIns.load(std::memory_order_relaxed);
			rd		+= s->reads	.load(std::memory_order_relaxed);
			wr		+= s->writes.load(std::memory_order_relaxed);
		}
	}

    Out << std::dec << "\n=========== Cache-Sim Report ============\n";
    Out << "Total instructions       : " << totIns  << '\n';
    Out << "  memory instructions    : " << totMem  << '\n';
    Out << "    reads                : " << rd      << '\n';
    Out << "    writes               : " << wr      << "\n\n";

    uint64_t l1Acc=0,l1Miss=0;
    for(auto* c:L1){ l1Acc+=c->Accesses(); l1Miss+=c->Misses(); }

    Out << "L1 accesses              : "
              << l1Acc << "   misses: " << l1Miss
              << "   MPKI: " << std::fixed << std::setprecision(5)
              << (totIns? (1000.0*l1Miss)/totIns : 0.0) << '\n';

    Out << "L2 accesses              : " << L2->Accesses()
              << "   misses: " << L2->Misses()
			  << "   MPKI: " << std::fixed << std::setprecision(5)
			  << (totIns? (1000.0*L2->Misses())/totIns : 0.0) << '\n';
			  
	Out << "\n  Clist Accesses: " << clist_access     << " ("
		      << std::fixed << std::setprecision(5)
			  << ((float)clist_access / (float)L2->Misses()) * 100.0 << "%)"
			  << "\n  Unclist Accesses: " << unclist_access << " ("
			  << std::fixed << std::setprecision(5)
			  << ((float)unclist_access / (float)L2->Misses()) * 100.0 << "%)"
		      << "\n  Cpage   Accesses: " << cpage_access   << " ("
			  << std::fixed << std::setprecision(5)
			  << ((float)cpage_access / (float)L2->Misses()) * 100.0 << "%)"
			  << std::endl;
    Out << "==========================================\n";

    delete L2;   // tidy
	delete unclist;
	delete clist;
}

// -----------------------------------------------------------------------
// Main method, execution + params here
// -----------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if(PIN_Init(argc, argv)){
        Out << "Pin init failed\n";
        return 1;
    }

	// Setting up knobs
	uint32_t unclsize 	  = KnobUncompressedListSize.Value();
	uint32_t clsize   	  = KnobCompressedListSize.Value();
	unclist_freq		  = KnobPromoteUncompressedFrequency.Value();
	clist_freq	  		  = KnobPromoteCompressedFrequency.Value();
	Out.open(KnobOutfile.Value());
	
	// Initializing page doubly linked lists
	clist   = new HASHLL::HashLL(clsize);
	unclist = new HASHLL::HashLL(unclsize);
	expansionFrequency = KnobExpansionFrequency.Value();

	/* 
		Clist and unclist sizes are parameters... we need to measure RSS for those.
		Is there a means of determining the RSS of the program that is being run
		THROUGH Pin?	
	*/

    cfgL1 = { KnobL1Size.Value(), KnobBlkBytes.Value(), KnobL1Assoc.Value() };
    cfgL2 = { KnobL2Size.Value(), KnobBlkBytes.Value(), KnobL2Assoc.Value() };
    L2    = new SimpleCache(cfgL2);                     // ← constructed *after* knobs parsed

    PIN_InitLock(&l2Lock);
	PIN_InitLock(&reset_lock);
	PIN_InitLock(&unc_lock);
	PIN_InitLock(&c_lock);
	PIN_InitLock(&cpage_lock);

    INS_AddInstrumentFunction(Instruction,  nullptr);
    PIN_AddThreadStartFunction(ThreadStart, nullptr);
    PIN_AddThreadFiniFunction (ThreadFini,  nullptr);
    PIN_AddFiniFunction       (Fini,        nullptr);

    PIN_StartProgram();    // never returns
    return 0;
}
