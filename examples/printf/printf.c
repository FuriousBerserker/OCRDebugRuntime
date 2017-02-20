#include "ocr.h"

ocrGuid_t mainEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    PRINTF("Hello from mainEdt()\n");
    PRINTF("%u, %p, %u, %p\n", paramc, paramv, depc, depv);
	ocrShutdown();
    return 0;
}
