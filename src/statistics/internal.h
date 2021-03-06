/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifdef OCR_ENABLE_STATISTICS

#ifndef __STATISTICS_INTERNAL_H__
#define __STATISTICS_INTERNAL_H__

#include "ocr-statistics.h"
#include "ocr-types.h"


/**
 * @brief Create a Stats Process
 * @param processGuid GUID of the process to which this ocrStatsProcess_t
 * will be associated to
 *
 * @return A ocrStatsProcess_t properly initialized
 */
ocrStatsProcess_t* intCreateStatsProcess(ocrGuid_t processGuid);

/**
 * @brief Destroy a StatsProcess
 *
 * This will also make sure that all pending messages
 * are processed
 *
 * @param self The ocrStatsProcess_t to destroy
 */
void intDestructStatsProcess(ocrStatsProcess_t *self);

/**
 * @brief Register a filter to respond to the messages
 * indicated by the bit mask
 *
 * When a process receives a "message" (in Lamport terminology),
 * it will inform all the filters registered with the type of message
 * received and then free the message. Messages will always be passed in
 * order to the filters but there is no guarantee as to what order
 * the filters will receive the messages
 *
 * @param self      The ocrStatsProcess_t to register with
 * @param bitMask   Bitmask of ocrStatsEvt_t that filter responds to
 * @param filter    Filter to pass the message to
 * @param out       If non-zero, this will register filters on the *sending*
 *                  of messages (ie: we will get notified if we are the *source*
 *                  of the message as opposed to the destination)
 *
 * @warn A filter should be registered to AT MOST one process
 * @warn Filter registration is not "thread-safe" so it should be done
 * only on process creation
 */
void intStatsProcessRegisterFilter(ocrStatsProcess_t *self, u64 bitMask,
                                   ocrStatsFilter_t *filter, u8 out);

/**
 * @brief Process a message that is being sent by src
 *
 * When a message gets sent by an agent, filters can be added
 * to be notified therefore providing a "source" view of events (ie: not
 * just what other agents do to me but what I do to others)
 * Note that the clock of the source is *not* updated here; this is
 * just a nice way to inform the source that it is sending a message
 *
 * @param src        Source of the message
 * @param msg        Message being sent
 *
 * @return 0 on success
 */
u8 intProcessOutgoingMessage(ocrStatsProcess_t *src, ocrStatsMessage_t* msg);

/**
 * @brief Process a message from the head of dst
 *
 * Returns 0 if no messages on the queue and 1 if a
 * message was processed
 *
 * @param dst       ocrProcess_t to process messages for
 * @return 0 if no message processed and 1 otherwise
 * @warn This function is *not* thread-safe. Admission control
 * must ensure that only one worker calls this at any given point (although
 * multiple workers can call it over time)
 */
u8 intProcessMessage(ocrStatsProcess_t *dst);

#endif /* __STATISTICS_INTERNAL_H__ */
#endif /* OCR_ENABLE_STATISTICS */
