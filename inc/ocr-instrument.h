/*
 * ocr-instrument.h
 *
 *  Created on: Feb 20, 2017
 *      Author: Lechen Yu
 *
 *  These empty functions are just placeholders called after certain system event
 *  occurs. These functions will be replaced by PIN dynamically to get the input
 *  parameter for future analysis.
 */

#ifndef __OCR_INSTRUMENT_H__
#define __OCR_INSTRUMENT_H__
#ifdef __cplusplus
extern "C" {
#endif

#include "ocr-types.h"
#include "ocr-version.h"
#include "extensions/ocr-runtime-itf.h"

void notifyEdtCreate(ocrGuid_t guid, ocrGuid_t templateGuid,
                u32 paramc, u64* paramv, u32 depc, ocrGuid_t *depv,
                u16 properties, ocrGuid_t outputEvent);

void notifyDbCreate(ocrGuid_t guid, void* addr, u64 len, u16 flags,
               ocrInDbAllocator_t allocator);

void notifyEventCreate(ocrGuid_t guid, ocrEventTypes_t eventType, u16 properties);

void notifyAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot,
        ocrDbAccessMode_t mode);

void notifyEventSatisfy(ocrGuid_t edtGuid, ocrGuid_t eventGuid, ocrGuid_t dataGuid, u32 slot);

void notifyShutdown();

void notifyEdtStart(ocrGuid_t edtGuid, u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[], u64* dbSizev);

#ifdef __cplusplus
}
#endif
#endif /* __OCR_INSTRUMENT_H__ */
