#include "ocr-instrument.h"
//#include "stdlib.h"
void notifyEdtCreate(ocrGuid_t guid, ocrGuid_t templateGuid,
                u32 paramc, u64* paramv, u32 depc, ocrGuid_t *depv,
                u16 properties, ocrGuid_t outputEvent, ocrGuid_t parent) {

}

void notifyDbCreate(ocrGuid_t guid, void* addr, u64 len, u16 flags,
               ocrInDbAllocator_t allocator) {

}

void notifyEventCreate(ocrGuid_t guid, ocrEventTypes_t eventType, u16 properties) {

}

void notifyAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
        ocrDbAccessMode_t mode) {

}

void notifyEventSatisfy(ocrGuid_t edtGuid, ocrGuid_t eventGuid, ocrGuid_t dataGuid, u32 slot) {

}

void notifyShutdown() {

}

void notifyEdtStart(ocrGuid_t edtGuid, u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[], u64* dbSizev) {

}

void notifyDbRelease(ocrGuid_t edtGuid, ocrGuid_t dbGuid) {

}

void notifyDbDestroy(ocrGuid_t dbGuid) {

}

void notifyEventPropagate(ocrGuid_t eventGuid) {

}


