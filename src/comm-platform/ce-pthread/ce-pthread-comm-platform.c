/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_COMM_PLATFORM_CE_PTHREAD

#include "debug.h"

#include "ocr-policy-domain.h"

#include "ocr-errors.h"
#include "ocr-sysboot.h"
#include "ocr-hal.h"
#include "utils/ocr-utils.h"

#include "ce-pthread-comm-platform.h"
#include "policy-domain/ce/ce-policy.h"

#define DEBUG_TYPE COMM_PLATFORM

void cePthreadCommDestruct (ocrCommPlatform_t * base) {
    runtimeChunkFree((u64)base, NULL);
}

void cePthreadCommBegin(ocrCommPlatform_t * commPlatform, ocrPolicyDomain_t * PD, ocrCommApi_t *comm) {
    u32 i, idx, numChannels, xeCount;
    ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*) commPlatform;
    commPlatform->pd = PD;
    commPlatformCePthread->numNeighborChannels = 0;
    xeCount = ((ocrPolicyDomainCe_t*)PD)->xeCount;
    numChannels = xeCount + PD->neighborCount;
    commPlatformCePthread->numChannels = numChannels;
    commPlatformCePthread->numNeighborChannels = PD->neighborCount;
    commPlatformCePthread->channelIdx = 0;
    commPlatformCePthread->channels = (ocrCommChannel_t*)runtimeChunkAlloc(numChannels * sizeof(ocrCommChannel_t), PERSISTENT_CHUNK);
    commPlatformCePthread->neighborChannels = (ocrCommChannel_t**)runtimeChunkAlloc(commPlatformCePthread->numNeighborChannels * sizeof(ocrCommChannel_t*), PERSISTENT_CHUNK);
    for (i = 0; i < numChannels; i++) {
        DPRINTF(DEBUG_LVL_VVERB, "Setting up channel %d: %p \n", i, &(commPlatformCePthread->channels[i]));
        commPlatformCePthread->channels[i].message = NULL;
        commPlatformCePthread->channels[i].remoteCounter = 0;
        commPlatformCePthread->channels[i].localCounter = 0;
        commPlatformCePthread->channels[i].msgCancel = false;
        initializePolicyMessage(&(commPlatformCePthread->channels[i].messageBuffer), sizeof(ocrPolicyMsg_t));
        initializePolicyMessage(&(commPlatformCePthread->channels[i].overwriteBuffer), sizeof(ocrPolicyMsg_t));
    }

    for (i = 0, idx = xeCount; i < PD->neighborCount; i++) {
        commPlatformCePthread->channels[idx++].remoteLocation = PD->neighbors[i];
    }

    // Setup neighbor translation table
    u64 parentSlotRequired = 1;
    for(i = 0; i < PD->neighborCount; i++) {
        if (PD->neighbors[i] == PD->parentLocation) {
            parentSlotRequired = 0;
            commPlatformCePthread->parentIdx = i;
            break;
        }
    }

    commPlatformCePthread->countTT = PD->neighborCount + xeCount + parentSlotRequired;
    commPlatformCePthread->locationTT = (ocrNeighbor_t*)runtimeChunkAlloc(commPlatformCePthread->countTT * sizeof(ocrNeighbor_t), PERSISTENT_CHUNK);
    for(i = 0; i < PD->neighborCount; i++) {
        commPlatformCePthread->locationTT[i].loc = PD->neighbors[i];
        commPlatformCePthread->locationTT[i].seqId = UNINITIALIZED_NEIGHBOR_INDEX;
    }

    for(i = 0; i < xeCount; i++) {
        u64 idx = PD->neighborCount + i;
        commPlatformCePthread->locationTT[idx].loc = PD->myLocation - xeCount + i;
        commPlatformCePthread->locationTT[idx].seqId = UNINITIALIZED_NEIGHBOR_INDEX;
    }

    if(parentSlotRequired == 1) {
        commPlatformCePthread->parentIdx = PD->neighborCount + xeCount;
        commPlatformCePthread->locationTT[commPlatformCePthread->parentIdx].loc = PD->parentLocation;
        commPlatformCePthread->locationTT[commPlatformCePthread->parentIdx].seqId = UNINITIALIZED_NEIGHBOR_INDEX;
    }
    return;
}

void cePthreadCommStart(ocrCommPlatform_t * commPlatform, ocrPolicyDomain_t * PD, ocrCommApi_t *comm) {
    u32 i, j;
    ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*) commPlatform;
    for (i = 0; i < PD->neighborCount; i++) {
        ocrPolicyDomain_t * neighborPD = PD->neighborPDs[i];
        ocrCommPlatformCePthread_t * commPlatformCePthreadNeighbor = (ocrCommPlatformCePthread_t *)neighborPD->commApis[0]->commPlatform;
        ocrCommChannel_t *neighborChannel = NULL;
        for (j = 0; j < commPlatformCePthreadNeighbor->numChannels; j++) {
            if (commPlatformCePthreadNeighbor->channels[j].remoteLocation == PD->myLocation) {
                neighborChannel = &(commPlatformCePthreadNeighbor->channels[j]);
                break;
            }
        }
        ASSERT(neighborChannel != NULL);
        DPRINTF(DEBUG_LVL_VVERB, "Setting up neighbor channel %d pointer %p \n", i, neighborChannel);
        commPlatformCePthread->neighborChannels[i] = neighborChannel;
    }
    return;
}

void cePthreadCommStop(ocrCommPlatform_t * commPlatform, ocrRunLevel_t newRl, u32 action) {
    // Nothing to do really
}

u8 cePthreadCommCheckSeqIdRecv(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {
    if (msg->seqId == UNINITIALIZED_NEIGHBOR_INDEX) {
        //This is done until registration completes
        u64 i;
        ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*)self;
        for (i = 0 ; i < commPlatformCePthread->countTT; i++) {
            if (commPlatformCePthread->locationTT[i].loc == msg->srcLocation) {
                msg->seqId = i;
                break;
            }
        }
        ASSERT(msg->seqId != UNINITIALIZED_NEIGHBOR_INDEX);
    }
    return 0;
}

u8 cePthreadCommSendMessage(ocrCommPlatform_t *self, ocrLocation_t target, ocrPolicyMsg_t *msg,
                            u64 *id, u32 properties, u32 mask) {
    u32 i;
    ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*)self;
    ocrPolicyDomainCe_t *cePD = (ocrPolicyDomainCe_t*)self->pd;
    ASSERT(target != self->pd->myLocation);
    msg->seqId = self->fcts.getSeqIdAtNeighbor(self, target, msg->seqId);

    // BUG #135: This is currently a hack to know if target is a CE or XE.
    // This needs to be replaced by a location query in future.
    ocrCommChannel_t * channel = NULL;
    bool ceTarget = false;
    for (i = 0; i < self->pd->neighborCount; i++) {
        if (target == self->pd->neighbors[i]) {
            ceTarget = true;
            break;
        }
    }

    if (ceTarget) {
        //CE --> CE: Non-blocking sends over a single buffer
        if (msg->type & PD_MSG_REQUEST) {
            ASSERT(!(msg->type & PD_MSG_RESPONSE));
            for (i = 0; i < commPlatformCePthread->numNeighborChannels; i++) {
                if (commPlatformCePthread->neighborChannels[i]->remoteLocation == self->pd->myLocation) {
                    channel = commPlatformCePthread->neighborChannels[i];
                    break;
                }
            }
            ASSERT(channel);
            DPRINTF(DEBUG_LVL_VVERB, "Send CE REQ Type: ix%x Dest: %lu Channel: %p RemoteCounter: %lu LocalCounter: %lu\n",
                    msg->type, target, channel, channel->remoteCounter, channel->localCounter);
            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, 0);
            hal_fence();
            ocrPolicyMsg_t * channelMessage = (ocrPolicyMsg_t*)channel->message;
            if (channelMessage == NULL) {
                ASSERT(channel->messageBuffer.bufferSize >= baseSize + marshalledSize);
                ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)(&(channel->messageBuffer)), MARSHALL_FULL_COPY);
                channel->message = &(channel->messageBuffer);
                hal_fence();
                ++(channel->remoteCounter);
            } else if ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_RL_NOTIFY && channelMessage != &(channel->overwriteBuffer)) {
                //Special case for shutdown: Overwrite any exisiting messages in the buffer
                ASSERT(channel->overwriteBuffer.bufferSize >= baseSize + marshalledSize);
                ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)(&(channel->overwriteBuffer)), MARSHALL_FULL_COPY);
                if (!__sync_bool_compare_and_swap((&(channel->message)), channelMessage, &(channel->overwriteBuffer))) {
                    ASSERT(channel->message == NULL);
                    channel->message = &(channel->overwriteBuffer);
                    hal_fence();
                    ++(channel->remoteCounter);
                }
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Send CE Busy Error REQ Type: 0x%x Dest: %lu Channel: %p RemoteCounter: %lu LocalCounter: %lu\n",
                        msg->type, target, channel, channel->remoteCounter, channel->localCounter);
                return OCR_EBUSY;
            }

            if (properties & BLOCKING_SEND_MSG_PROP) {
                do {
                    hal_fence();
                } while(channel->remoteCounter > channel->localCounter);
            }
        } else {
            ASSERT(msg->type & PD_MSG_RESPONSE);
            for (i = cePD->xeCount; i < commPlatformCePthread->numChannels; i++) {
                if (commPlatformCePthread->channels[i].remoteLocation == target) {
                    channel = &(commPlatformCePthread->channels[i]);
                    break;
                }
            }
            ASSERT(channel && channel->message == NULL);
            DPRINTF(DEBUG_LVL_VVERB, "Send CE RESP Type: 0x%x Dest: %lu Channel: %p RemoteCounter: %lu LocalCounter: %lu\n",
                    msg->type, target, channel, channel->remoteCounter, channel->localCounter);
            hal_fence();
            u64 baseSize = 0, marshalledSize = 0;
            ocrPolicyMsgGetMsgSize(msg, &baseSize, &marshalledSize, 0);
            ASSERT(channel->messageBuffer.bufferSize >= baseSize + marshalledSize);
            ocrPolicyMsgMarshallMsg(msg, baseSize, (u8*)(&(channel->messageBuffer)), MARSHALL_FULL_COPY);
            channel->message = &(channel->messageBuffer);
            hal_fence();
            ++(channel->localCounter);
        }

        DPRINTF(DEBUG_LVL_VERB, "Sending message @ %p to %lu of type 0x%x\n",
                msg, (u64)target, msg->type);
    } else {
        //CE --> XE: Blocking sends over a single buffer
        u64 block_size = cePD->xeCount + 1;
        channel = &(commPlatformCePthread->channels[((u64)target) % block_size]);

        ocrPolicyMsg_t * channelMessage = (ocrPolicyMsg_t *)__sync_val_compare_and_swap((&(channel->message)), NULL, msg);
        if (channelMessage != NULL) {
            DPRINTF(DEBUG_LVL_VERB, "Cancelling CE msg @ %p of type 0x%x to %lu; channel msg @ %p of type 0x%x from %lu\n",
                    msg, msg->type, msg->srcLocation, channelMessage, channelMessage->type,
                    channelMessage->srcLocation);
            RESULT_TRUE(__sync_bool_compare_and_swap((&(channel->message)), channelMessage, msg));
            RESULT_TRUE(__sync_bool_compare_and_swap((&(channel->msgCancel)), false, true));
            hal_fence();
            ++(channel->localCounter);
        }
        DPRINTF(DEBUG_LVL_VERB, "Sending message @ %p of type 0x%x to %lu\n",
                msg, msg->type, target);
        hal_fence();
        ++(channel->localCounter);
        do {
            hal_fence();
        } while(channel->localCounter > channel->remoteCounter);
    }

    return 0;
}

u8 cePthreadCommPollMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                            u32 properties, u32 *mask) {
    u32 i, startIdx, numChannels;
    u64 localCounter, remoteCounter;
    ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*)self;

    //Poll for [CE] responses (serve your own self first :-))
    for (i = 0; i < commPlatformCePthread->numNeighborChannels; i++) {
        ocrCommChannel_t * channel = commPlatformCePthread->neighborChannels[i];
        if (channel->localCounter > channel->remoteCounter) {
            ocrPolicyMsg_t * message = (ocrPolicyMsg_t*)channel->message;
            ASSERT(message != NULL);
            RESULT_TRUE(__sync_bool_compare_and_swap((&(channel->message)), message, NULL));
            u64 baseSize, marshalledSize;
            ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, 0);
            ASSERT(message->bufferSize >= baseSize + marshalledSize);
            ocrPolicyMsgUnMarshallMsg((u8*)message, NULL, message, MARSHALL_APPEND);
            hal_fence();
            ++(channel->remoteCounter);
            *msg = message;
            cePthreadCommCheckSeqIdRecv(self, *msg);
            return 0;
        }
    }

    //Poll for [XE,CE] requests
    startIdx = commPlatformCePthread->startIdx;
    numChannels = commPlatformCePthread->numChannels;
    for (i = 0; i < numChannels; i++) {
        u32 idx = (startIdx + i) % numChannels;
        ocrCommChannel_t * channel = &(commPlatformCePthread->channels[idx]);
        localCounter = (u64)channel->localCounter;
        remoteCounter = (u64)channel->remoteCounter;
        ocrPolicyMsg_t * message = NULL;
        if (localCounter < remoteCounter) {
            do {//get a stable message; the original message might get overwritten by shutdown request
                message = (ocrPolicyMsg_t *)channel->message;
                remoteCounter = (u64)channel->remoteCounter;
            } while (!__sync_bool_compare_and_swap((&(channel->message)), message, NULL));
            ASSERT(message != NULL && remoteCounter == (localCounter + 1));
            if (message->type & PD_MSG_REQ_RESPONSE) {
                DPRINTF(DEBUG_LVL_VVERB, "Message from %u needs a response... using buffer @ %p\n",
                        idx, message);
                *msg = message;
            } else {
                u64 baseSize = 0, marshalledSize = 0;
                ocrPolicyMsgGetMsgSize(message, &baseSize, &marshalledSize, 0);
                ASSERT(channel->messageBuffer.bufferSize >= baseSize + marshalledSize);
                if(idx >= ((ocrPolicyDomainCe_t*)(self->pd))->xeCount) {
                    // Message coming from another CE. It uses one of these buffers
                    if(message == &(channel->overwriteBuffer)) {
                        DPRINTF(DEBUG_LVL_VVERB, "Message from CE %u one-way (overwriteBuffer)... copying from %p to %p (sz: %lu)\n",
                                idx, message, &(channel->messageBuffer), baseSize + marshalledSize);
                        ocrPolicyMsgUnMarshallMsg((u8*)(&(channel->overwriteBuffer)), NULL, &(channel->messageBuffer),
                                                  MARSHALL_FULL_COPY);
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB, "Message from CE %u one-way... no copy (buffer @ %p)\n",
                                message);
                        ASSERT(message == &(channel->messageBuffer));
                    }
                } else {
                    // Message coming from an XE
                    DPRINTF(DEBUG_LVL_VVERB, "Message from XE %u one-way... copying from %p to %p (sz: %lu)\n",
                            idx, message, &(channel->messageBuffer), baseSize + marshalledSize);
                    ocrPolicyMsgMarshallMsg(message, baseSize, (u8*)(&(channel->messageBuffer)), MARSHALL_DUPLICATE);
                }
                *msg = &(channel->messageBuffer);
            }

            hal_fence();
            ++(channel->localCounter);
            commPlatformCePthread->startIdx = (idx + 1) % numChannels;
            DPRINTF(DEBUG_LVL_VERB, "Received message @ %p of type 0x%x from %lu\n",
                    (*msg), (*msg)->type, (u64)((*msg)->srcLocation));
            cePthreadCommCheckSeqIdRecv(self, *msg);
            return 0;
        }
    }
    return OCR_EAGAIN;
}

u8 cePthreadCommWaitMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t **msg,
                            u32 properties, u32 *mask) {
    while (cePthreadCommPollMessage(self, msg, properties, mask) != 0)
        ;
    return 0;
}

u64 cePthreadGetSeqIdAtNeighbor(ocrCommPlatform_t *self, ocrLocation_t neighborLoc, u64 neighborId) {
    ocrCommPlatformCePthread_t *commPlatformCePthread = (ocrCommPlatformCePthread_t*)self;
    if (neighborLoc == self->pd->parentLocation) {
        return commPlatformCePthread->locationTT[commPlatformCePthread->parentIdx].seqId;
    }
    return commPlatformCePthread->locationTT[neighborId].seqId;
}

u8 cePthreadDestructMessage(ocrCommPlatform_t *self, ocrPolicyMsg_t *msg) {
    //BUG #527: memory reclaim: destruct message
    return 0;
}

ocrCommPlatform_t* newCommPlatformCePthread(ocrCommPlatformFactory_t *factory,
        ocrParamList_t *perInstance) {

    ocrCommPlatformCePthread_t * commPlatformCePthread = (ocrCommPlatformCePthread_t*)
            runtimeChunkAlloc(sizeof(ocrCommPlatformCePthread_t), PERSISTENT_CHUNK);
    ocrCommPlatform_t * base = (ocrCommPlatform_t *) commPlatformCePthread;
    factory->initialize(factory, base, perInstance);
    return base;
}

void initializeCommPlatformCePthread(ocrCommPlatformFactory_t * factory, ocrCommPlatform_t * base, ocrParamList_t * perInstance) {
    initializeCommPlatformOcr(factory, base, perInstance);

    ocrCommPlatformCePthread_t *commPlatformCePthread = (ocrCommPlatformCePthread_t*)base;
    commPlatformCePthread->channels = NULL;
}

/******************************************************/
/* OCR COMP PLATFORM PTHREAD FACTORY                  */
/******************************************************/

void destructCommPlatformFactoryCePthread(ocrCommPlatformFactory_t *factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrCommPlatformFactory_t *newCommPlatformFactoryCePthread(ocrParamList_t *perType) {
    ocrCommPlatformFactory_t *base = (ocrCommPlatformFactory_t*)
                                     runtimeChunkAlloc(sizeof(ocrCommPlatformFactoryCePthread_t), NONPERSISTENT_CHUNK);

    base->instantiate = &newCommPlatformCePthread;
    base->initialize = &initializeCommPlatformCePthread;
    base->destruct = &destructCommPlatformFactoryCePthread;

    base->platformFcts.destruct = FUNC_ADDR(void (*)(ocrCommPlatform_t*), cePthreadCommDestruct);
    base->platformFcts.begin = FUNC_ADDR(void (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrCommApi_t *),
                                         cePthreadCommBegin);
    base->platformFcts.start = FUNC_ADDR(void (*)(ocrCommPlatform_t*, ocrPolicyDomain_t*, ocrCommApi_t *),
                                         cePthreadCommStart);
    base->platformFcts.stop = FUNC_ADDR(void (*)(ocrCommPlatform_t*,ocrRunLevel_t,u32), cePthreadCommStop);
    base->platformFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrLocation_t, ocrPolicyMsg_t *, u64*, u32, u32),
                                     cePthreadCommSendMessage);
    base->platformFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t **, u32, u32*),
                                     cePthreadCommPollMessage);
    base->platformFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t **, u32, u32*),
                                     cePthreadCommWaitMessage);
    base->platformFcts.destructMessage = FUNC_ADDR(u8 (*)(ocrCommPlatform_t*, ocrPolicyMsg_t*),
                                                   cePthreadDestructMessage);
    base->platformFcts.getSeqIdAtNeighbor = FUNC_ADDR(u64 (*)(ocrCommPlatform_t*, ocrLocation_t, u64),
                                                   cePthreadGetSeqIdAtNeighbor);

    return base;
}
#endif /* ENABLE_COMM_PLATFORM_CE_PTHREAD */