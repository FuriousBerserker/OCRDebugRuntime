#include "ocr-config.h"
#ifdef SAL_FSIM_XE

#include "debug.h"
#include "ocr-types.h"

#include "xstg-arch.h"
#include "mmio-table.h"

#define DEBUG_TYPE SAL

void salPdDriver(void* pdVoid) {
    ocrPolicyDomain_t *pd = (ocrPolicyDomain_t*)pdVoid;

    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_CONFIG_PARSE, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_NETWORK_OK, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_PD_OK, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_MEMORY_OK, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_GUID_OK, RL_REQUEST | RL_BARRIER
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_COMPUTE_OK, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_USER_OK, RL_REQUEST | RL_ASYNC
                                          | RL_BRING_UP | RL_NODE_MASTER), ==, 0);

    // When we come back here, we continue bring down from GUID_OK. The switchRunlevel
    // takes care of bringing us out down through RL_COMPUTE_OK. In particular, both
    // shutdown barriers are traversed when we get here so we don't have to worry
    // about that
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_GUID_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                  ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_MEMORY_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                  ==, 0);

    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_PD_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                  ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_NETWORK_OK, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                  ==, 0);
    RESULT_ASSERT(pd->fcts.switchRunlevel(pd, RL_CONFIG_PARSE, RL_REQUEST | RL_ASYNC | RL_TEAR_DOWN | RL_NODE_MASTER),
                  ==, 0);

    hal_exit(0);
    return;
}

/* NOTE: Below functions are placeholders for platform independence.
 *       Currently no functionality on tg.
 */

u32 salPause(bool isBlocking){
    DPRINTF(DEBUG_LVL_VERB, "ocrPause/ocrQuery/ocrResume not yet supported on tg\n");
    return 1;
}

ocrGuid_t salQuery(ocrQueryType_t query, ocrGuid_t guid, void **result, u32 *size, u8 flags){
     return NULL_GUID;
}

void salResume(u32 flag){
     return;

}

u64 salGetTime(void){
    u64 cycles = 0;
#if !defined(ENABLE_BUILDER_ONLY)
    cycles = *(u64 *)(AR_MSR_BASE + GLOBAL_TIME_STAMP_COUNTER * sizeof(u64));
#endif
    return cycles;
}

#ifdef ENABLE_EXTENSION_PERF

u64 pmuCounters[] = {
    78, //CORE_STALL_CYCLES,
    64, //INSTRUCTIONS_EXECUTED,
     6, //LOCAL_READ_COUNT,
    14, //LOCAL_WRITE_COUNT,
    8,  //REMOTE_READ_COUNT,
    16, //REMOTE_WRITE_COUNT,
    0
};

u64 salPerfInit(salPerfCounter* perfCtr) {
    // Does nothing, added for compatibility
    return 0;
}

u64 salPerfStart(salPerfCounter* perfCtr) {
    u32 i = 0;

    for(i = 0; pmuCounters[i]; i++)
        // reset & enable the PMU counters
        *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[i]) = 0;

    return 0;
}

u64 salPerfStop(salPerfCounter* perfCtr) {

    // Doesn't really stop, just reads the counter values
    perfCtr[PERF_HW_CYCLES].perfVal = *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[0]) +
                              *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[1]);
    perfCtr[PERF_L1_HITS].perfVal = *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[2]) +
                              *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[3]);
    perfCtr[PERF_L1_MISSES].perfVal = *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[4]) +
                              *(u64 *)(AR_PMU_BASE + sizeof(u64)*pmuCounters[5]);
    perfCtr[PERF_FLOAT_OPS].perfVal = 0xdeaddead;

    return 0;
}

u64 salPerfShutdown(salPerfCounter *perfCtr) {
    // Does nothing, added for compatibility
    return 0;
}

#endif

#ifdef TG_GDB_SUPPORT
void __xeDoAssert(const char* fn, u32 ln) {
    DPRINTF(DEBUG_LVL_WARN, "ASSERT FAILURE XE at line %"PRId32" in '%s'\n", (int)(ln), fn);
    DPRINTF(DEBUG_LVL_WARN, "GDB should break here for assert\n");
}
#endif

#endif /* ENABLE_POLICY_DOMAIN_XE */
