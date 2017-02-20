/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef __OCR_POLICY_MSG_LIST_H__

// WARNING: Keep in sync with ocr-policy-domain.h
PER_TYPE(PD_MSG_DB_CREATE)
PER_TYPE(PD_MSG_DB_DESTROY)
PER_TYPE(PD_MSG_DB_ACQUIRE)
PER_TYPE(PD_MSG_DB_RELEASE)
PER_TYPE(PD_MSG_DB_FREE)

PER_TYPE(PD_MSG_MEM_ALLOC)
PER_TYPE(PD_MSG_MEM_UNALLOC)

PER_TYPE(PD_MSG_WORK_CREATE)
PER_TYPE(PD_MSG_WORK_EXECUTE)
PER_TYPE(PD_MSG_WORK_DESTROY)

PER_TYPE(PD_MSG_EDTTEMP_CREATE)
PER_TYPE(PD_MSG_EDTTEMP_DESTROY)

PER_TYPE(PD_MSG_EVT_CREATE)
PER_TYPE(PD_MSG_EVT_DESTROY)
PER_TYPE(PD_MSG_EVT_GET)

PER_TYPE(PD_MSG_GUID_CREATE)
PER_TYPE(PD_MSG_GUID_INFO)
PER_TYPE(PD_MSG_GUID_METADATA_CLONE)
PER_TYPE(PD_MSG_GUID_RESERVE)
PER_TYPE(PD_MSG_GUID_UNRESERVE)
PER_TYPE(PD_MSG_GUID_DESTROY)

PER_TYPE(PD_MSG_COMM_TAKE)
PER_TYPE(PD_MSG_COMM_GIVE)

PER_TYPE(PD_MSG_SCHED_GET_WORK)
PER_TYPE(PD_MSG_SCHED_NOTIFY)
PER_TYPE(PD_MSG_SCHED_TRANSACT)
PER_TYPE(PD_MSG_SCHED_ANALYZE)

PER_TYPE(PD_MSG_DEP_ADD)
PER_TYPE(PD_MSG_DEP_REGSIGNALER)
PER_TYPE(PD_MSG_DEP_REGWAITER)
PER_TYPE(PD_MSG_DEP_SATISFY)
PER_TYPE(PD_MSG_DEP_UNREGSIGNALER)
PER_TYPE(PD_MSG_DEP_UNREGWAITER)
PER_TYPE(PD_MSG_DEP_DYNADD)
PER_TYPE(PD_MSG_DEP_DYNREMOVE)

PER_TYPE(PD_MSG_SAL_PRINT)
PER_TYPE(PD_MSG_SAL_READ)
PER_TYPE(PD_MSG_SAL_WRITE)
PER_TYPE(PD_MSG_SAL_TERMINATE)

PER_TYPE(PD_MSG_MGT_REGISTER)
PER_TYPE(PD_MSG_MGT_UNREGISTER)
PER_TYPE(PD_MSG_MGT_MONITOR_PROGRESS)
PER_TYPE(PD_MSG_MGT_RL_NOTIFY)

PER_TYPE(PD_MSG_HINT_SET)
PER_TYPE(PD_MSG_HINT_GET)

#endif /* __OCR_POLICY_MSG_LIST_H__ */
