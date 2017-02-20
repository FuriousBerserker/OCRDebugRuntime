/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_MACHINE_H_

#define __OCR_MACHINE_H_

#include "external/iniparser.h"
#include "ocr-allocator.h"
#include "ocr-comp-platform.h"
#include "ocr-comp-target.h"
#include "ocr-datablock.h"
#include "ocr-event.h"
#include "ocr-mem-platform.h"
#include "ocr-mem-target.h"
#include "ocr-policy-domain.h"
#include "ocr-scheduler.h"
#include "ocr-worker.h"
#include "ocr-workpile.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>


typedef enum {
    guid_type,
    memplatform_type,
    memtarget_type,
    allocator_type,
    commapi_type,
    commplatform_type,
    compplatform_type,
    comptarget_type,
    workpile_type,
    worker_type,
    scheduler_type,
    schedulerObject_type,
    schedulerHeuristic_type,
    policydomain_type,
    // Everything above is used as instances
    // Everything below is used as types
    // WARNING: Order must match the order in ocr-policy-domain.h
    taskfactory_type, // WARNING: the code relies on the fact that this is the first factory type
    tasktemplatefactory_type,
    datablockfactory_type,
    eventfactory_type, // WARNING: the code relies on the fact that this is the last factory type
} type_enum;

/* Dependence information (from->to) referenced by refstr */

typedef struct {
    type_enum from;
    type_enum to;
    char *refstr;
} dep_t;

#endif
