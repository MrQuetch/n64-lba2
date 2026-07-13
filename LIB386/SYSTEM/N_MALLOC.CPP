#include <SYSTEM/N_MALLOC.H>

#include <stdlib.h>

#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/LOGPRINT.H>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#endif

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
#define MALLOC_MASK (~(MALLOC_ALIGN - 1))

// -----------------------------------------------------------------------------
#ifdef LBA2_TARGET_WIIU
// Private bump allocator on top of a 32 MB block we grab once at startup.
// Bypasses both libc malloc and MEMAllocFromDefaultHeapEx which together
// fail on small (~3 KB) requests after only 3 MB of engine allocations,
// for reasons we couldn't diagnose with heap probes / base-heap queries.
// V1: no Free support (just leak on NormFree). V2 will revisit if needed.
#define WIIU_BUMP_POOL_SIZE (32UL * 1024UL * 1024UL)
static U8 *s_bumpPool   = NULL;
static U32 s_bumpOffset = 0;
static U8  s_bumpInit   = 0;

static void WiiUBumpInit(void) {
    if (s_bumpInit) return;
    s_bumpInit = 1;
    s_bumpPool = (U8 *)MEMAllocFromDefaultHeapEx(WIIU_BUMP_POOL_SIZE, 32);
    LogPrintf("[bump] pool=%p size=%lu MB",
              s_bumpPool, (unsigned long)(WIIU_BUMP_POOL_SIZE/(1024UL*1024UL)));
}

static void *WiiUBumpAlloc(U32 size, U32 align) {
    if (!s_bumpInit) WiiUBumpInit();
    if (!s_bumpPool) return NULL;
    // Round offset up to alignment boundary.
    U32 mask = align - 1;
    U32 aligned = (s_bumpOffset + mask) & ~mask;
    if (aligned + size > WIIU_BUMP_POOL_SIZE) return NULL; // pool exhausted
    void *p = s_bumpPool + aligned;
    s_bumpOffset = aligned + size;
    return p;
}
#endif

void *NormMalloc(U32 size) {
    U64 addr, temp;

#ifdef LBA2_TARGET_WIIU
    temp = (U64)WiiUBumpAlloc(size + MALLOC_ALIGN, MALLOC_ALIGN);
#else
    temp = (U64)malloc(size + MALLOC_ALIGN);
#endif

#ifdef LBA2_TARGET_WIIU
    static U64 s_totalAllocated = 0;
    static U32 s_allocCount = 0;
    if (temp) {
        s_totalAllocated += size;
        s_allocCount++;
    }
    // Log requests >10KB and every failure, with running totals to catch
    // pressure or fragmentation patterns.
    if (!temp || size > (10UL * 1024UL)) {
        LogPrintf("[NormMalloc] size=%lu (%lu KB) total=%lu MB n=%lu -> %s",
                  (unsigned long)size,
                  (unsigned long)(size / 1024UL),
                  (unsigned long)(s_totalAllocated / (1024UL * 1024UL)),
                  (unsigned long)s_allocCount,
                  temp ? "OK" : "FAILED");
        if (!temp) {
            // On failure, dump MEM2 heap status so we see whether the heap
            // is genuinely exhausted or if the request just hit a limit.
            MEMHeapHandle h2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
            if (h2) {
                uint32_t f = MEMGetTotalFreeSizeForExpHeap(h2);
                uint32_t a = MEMGetAllocatableSizeForExpHeapEx(h2, 4);
                LogPrintf("  [fail] MEM2 free=%lu KB largest=%lu KB",
                          (unsigned long)(f/1024UL), (unsigned long)(a/1024UL));
            }
        }
    }
#endif

    if (!temp) {
        return NULL;
    }

    addr = (temp + MALLOC_ALIGN) & MALLOC_MASK;

    *((S8 *)addr - 2) = 0;
    *((S8 *)addr - 1) = (S8)(addr - temp);

    return (void *)addr;
}

void NormFree(void *addr) {
    addr = (void *)((U64)addr - *((S8 *)addr - 1));
#ifdef LBA2_TARGET_WIIU
    // V1 bump allocator: NO-OP (we leak). Engine init doesn't free much,
    // and the 32 MB pool is more than enough for V1 boot scope.
    (void)addr;
#else
    free(addr);
#endif
}

void *NormMshrink(void *addr, U32 size) {
    // There is no portable equivalent to '_expand' without changing
    //   code that uses this function, making it fail is valid according to
    //   the "contract" of the function
    return NULL;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
