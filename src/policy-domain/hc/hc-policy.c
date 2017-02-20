/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */


#include "ocr-config.h"
#ifdef ENABLE_POLICY_DOMAIN_HC

#include "debug.h"
#include "ocr-errors.h"
#include "ocr-db.h"
#include "ocr-policy-domain.h"
#include "ocr-sysboot.h"
#include "ocr-runtime-hints.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#endif

#ifdef ENABLE_EXTENSION_LABELING
#include "experimental/ocr-labeling-runtime.h"
#endif

#include "utils/profiler/profiler.h"

#include "policy-domain/hc/hc-policy.h"
#include "allocator/allocator-all.h"

//BUG #204: cloning: hack to support edt templates, and pause\resume
#include "task/hc/hc-task.h"
#include "event/hc/hc-event.h"
#include "workpile/hc/hc-workpile.h"

// Currently required to find out if self is the blessed PD
#include "extensions/ocr-affinity.h"

#define DEBUG_TYPE POLICY

static u8 helperSwitchInert(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, phase_t phase, u32 properties) {
    u64 i = 0;
    u64 maxCount = 0;
    u8 toReturn = 0;
    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->commApis[i]->fcts.switchRunlevel(
            policy->commApis[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->guidProviders[i]->fcts.switchRunlevel(
            policy->guidProviders[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->allocators[i]->fcts.switchRunlevel(
            policy->allocators[i], policy, runlevel, phase, properties, NULL, 0);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        toReturn |= policy->schedulers[i]->fcts.switchRunlevel(
            policy->schedulers[i], policy, runlevel, phase, properties, NULL, 0);
    }

    return toReturn;
}

// Callback from the capable modules
// val contains worker id on lower 16 bits and RL on next 16 bits
void hcWorkerCallback(ocrPolicyDomain_t *self, u64 val) {
    ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)self;
    DPRINTF(DEBUG_LVL_VERB, "Got check-in from worker %u for RL %lu\n", val & 0xFFFF, (u64)(val >> 16));
    // Read these now and fence because on TEAR_DOWN as soon
    // as checkedIn reaches zero the master thread falls-through
    // and write to rlSwitch.
    ocrRunlevel_t runlevel = rself->rlSwitch.runlevel;
    s8 nextPhase = rself->rlSwitch.nextPhase;
    u32 properties = rself->rlSwitch.properties;
    hal_fence();
    u32 oldVal, newVal;
    do {
        oldVal = rself->rlSwitch.checkedIn;
        newVal = hal_cmpswap32(&(rself->rlSwitch.checkedIn), oldVal, oldVal - 1);
    } while(oldVal != newVal);
    if(oldVal == 1) {
        // This means we managed to set it to 0
        DPRINTF(DEBUG_LVL_VVERB, "All workers checked in, moving to the next stage: RL %u; phase %d\n",
                runlevel, nextPhase);
        if(properties & RL_FROM_MSG) {
            // We need to re-enter switchRunlevel
            if((properties & RL_BRING_UP) &&
               (nextPhase == RL_GET_PHASE_COUNT_UP(self, runlevel))) {
                // Switch to the next runlevel
                ++rself->rlSwitch.runlevel;
                rself->rlSwitch.nextPhase = 0;
            }
            if((properties & RL_TEAR_DOWN) && (nextPhase == -1)) {
                // Switch to the next runlevel (going down)
                --rself->rlSwitch.runlevel;
                rself->rlSwitch.nextPhase = RL_GET_PHASE_COUNT_DOWN(self, rself->rlSwitch.runlevel) - 1;
                hal_fence(); // for LEGACY MASTER loop probably not needed
            }
            // Ok to read cached value here since in this case, the previous
            // 'if' couldn't have updated next phase to zero.
            if(runlevel == RL_COMPUTE_OK && nextPhase == 0) {
                // In this case, we do not re-enter the switchRunlevel because the master worker
                // will drop out of its computation (at some point) and take over
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER thread will pick up for switch to RL_COMPUTE_OK\n");
            } else {
                DPRINTF(DEBUG_LVL_VVERB, "Re-entering switchRunlevel with RL %u; phase %u; prop 0x%x\n",
                        rself->rlSwitch.runlevel, rself->rlSwitch.nextPhase, rself->rlSwitch.properties);
                RESULT_ASSERT(self->fcts.switchRunlevel(self, rself->rlSwitch.runlevel, rself->rlSwitch.properties), ==, 0);
            }
        } else { // else, some thread is already in switchRunlevel and will be unblocked
            DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER thread will continue\n");
        }
    }
}

// Function to cause run-level switches in this PD
u8 hcPdSwitchRunlevel(ocrPolicyDomain_t *policy, ocrRunlevel_t runlevel, u32 properties) {
    s32 j;
    phase_t i=0, curPhase, phaseCount;
    u32 maxCount;

    u8 toReturn = 0;
    u32 origProperties = properties;
    u32 propertiesPreComputes = properties;

#define GET_PHASE(counter) curPhase = (properties & RL_BRING_UP)?counter:(phaseCount - counter - 1)

    ocrPolicyDomainHc_t* rself = (ocrPolicyDomainHc_t*)policy;
    // Check properties
    u32 amNodeMaster = properties & RL_NODE_MASTER;
    u32 amPDMaster = properties & RL_PD_MASTER;
    properties &= ~(RL_NODE_MASTER); // Strip out this from the rest; only valuable for the PD and some
                                     // specific workers

    u32 fromPDMsg = properties & RL_FROM_MSG;
    properties &= ~RL_FROM_MSG; // Strip this out from the rest; only valuable for the PD

    // This is important before computes (some modules may do something different for the first thread)
    propertiesPreComputes = properties;
    if(amPDMaster) propertiesPreComputes |= RL_PD_MASTER;

    if(!(fromPDMsg)) {
        // RL changes called directly through switchRunlevel should
        // only transition until PD_OK. After that, transitions should
        // occur using policy messages
        ASSERT(amNodeMaster || (runlevel <= RL_PD_OK));
        // If this is direct function call, it should only be a request
        ASSERT((properties & RL_REQUEST) && !(properties & (RL_RESPONSE | RL_RELEASE)))
    }

    switch(runlevel) {
    case RL_CONFIG_PARSE:
    {
        // Are we bringing the machine up
        if(properties & RL_BRING_UP) {
            for(i = 0; i < RL_MAX; ++i) {
                for(j = 0; j < RL_PHASE_MAX; ++j) {
                     // Everything has at least one phase on both up and down
                    policy->phasesPerRunlevel[i][j] = (1<<4) + 1;
                }
            }
            phaseCount = 2;
        } else {
            // Tear down
            phaseCount = policy->phasesPerRunlevel[RL_CONFIG_PARSE][0] >> 4;
        }
        // Both cases
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, propertiesPreComputes);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, propertiesPreComputes, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_CONFIG_PARSE(%d) phase %d failed: %d\n", propertiesPreComputes, curPhase, toReturn);
        }

        if((!toReturn) && (properties & RL_BRING_UP)) {
            // Coalesce the phasesPerRunLevel by taking the maximum
            for(i = 0; i < RL_MAX; ++i) {
                u32 finalCount = policy->phasesPerRunlevel[i][0];
                for(j = 1; j < RL_PHASE_MAX; ++j) {
                    // Deal with UP phase count
                    u32 newCount = 0;
                    newCount = (policy->phasesPerRunlevel[i][j] & 0xF) > (finalCount & 0xF)?
                        (policy->phasesPerRunlevel[i][j] & 0xF):(finalCount & 0xF);
                    // And now the DOWN phase count
                    newCount |= ((policy->phasesPerRunlevel[i][j] >> 4) > (finalCount >> 4)?
                        (policy->phasesPerRunlevel[i][j] >> 4):(finalCount >> 4)) << 4;
                    finalCount = newCount;
                }
                policy->phasesPerRunlevel[i][0] = finalCount;
            }
        }
        break;
    }
    case RL_NETWORK_OK:
    {
        // In this single PD implementation, nothing specific to do (just pass it down)
        // In general, this is when you setup communication
        phaseCount = ((policy->phasesPerRunlevel[RL_NETWORK_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, propertiesPreComputes);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, propertiesPreComputes, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_NETWORK_OK(%d) phase %d failed: %d\n", propertiesPreComputes, curPhase, toReturn);
        }
        break;
    }
    case RL_PD_OK:
    {
        // In this single PD implementation for x86, there is nothing specific to do
        // In general, you need to:
        //     - if not amNodeMaster, start a worker for this PD
        //     - that worker (or the master one) needs to then transition all inert modules to PD_OK
        phaseCount = ((policy->phasesPerRunlevel[RL_PD_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;

        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, propertiesPreComputes);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, propertiesPreComputes, NULL, 0);
            }
        }

        if((!toReturn) && (properties & RL_BRING_UP)) {
            //BUG #583: is it important to do that at the first phase or ?
            // if((properties & RL_BRING_UP) && RL_IS_FIRST_PHASE_UP(PD, RL_PD_OK, phase)) {
#ifdef ENABLE_EXTENSION_PAUSE
            registerSignalHandler();
#endif
            ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)policy;
            //Initialize pause/query/resume variables
            rself->pqrFlags.runtimePause = false;
            rself->pqrFlags.pauseCounter = 0;
            rself->pqrFlags.pausingWorker = -1;
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_PD_OK(%d) phase %d failed: %d\n", propertiesPreComputes, curPhase, toReturn);
        }
        break;
    }
    case RL_MEMORY_OK:
    {
        phaseCount = ((policy->phasesPerRunlevel[RL_MEMORY_OK][0]) >> ((properties&RL_TEAR_DOWN)?4:0)) & 0xF;
        maxCount = policy->workerCount;
        for(i = 0; i < phaseCount; ++i) {
            if(toReturn) break;
            GET_PHASE(i);
            toReturn |= helperSwitchInert(policy, runlevel, curPhase, propertiesPreComputes);
            for(j = 0; j < maxCount; ++j) {
                toReturn |= policy->workers[j]->fcts.switchRunlevel(
                    policy->workers[j], policy, runlevel, curPhase, propertiesPreComputes, NULL, 0);
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_MEMORY_OK(%d) phase %d failed: %d\n", propertiesPreComputes, curPhase, toReturn);
        }
        break;
    }
    case RL_GUID_OK:
    {
        // In the general case (with more than one PD), in this step and on bring-up:
        //     - send messages to all neighboring PDs to transition to this state
        //     - do local transition
        //     - wait for responses from neighboring PDs
        //     - report back to caller (if RL_FROM_MSG)
        //     - send release message to neighboring PDs
        // If this is RL_FROM_MSG, the above steps may occur in multiple steps (ie: you
        // don't actually "wait" but rather re-enter this function on incomming
        // messages from neighbors. If not RL_FROM_MSG, you do block.

        if(properties & RL_BRING_UP) {
            // On BRING_UP, bring up GUID provider
            // We assert that there are two phases. The first phase is mostly to bring
            // up the GUID provider and the last phase is to actually get GUIDs for
            // the various components if needed
            phaseCount = policy->phasesPerRunlevel[RL_GUID_OK][0] & 0xF;
            maxCount = policy->workerCount;

            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, propertiesPreComputes);
                for(j = 0; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, propertiesPreComputes, NULL, 0);
                }

            }
        } else {
            phaseCount = policy->phasesPerRunlevel[RL_GUID_OK][0] >> 4;
            maxCount = policy->workerCount;
            for(i = 0; i < phaseCount; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, propertiesPreComputes);
                for(j = 0; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, propertiesPreComputes, NULL, 0);
                }
            }
        }

        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_GUID_OK(%d) phase %d failed: %d\n", propertiesPreComputes, i-1, toReturn);
        }
        break;
    }

    case RL_COMPUTE_OK:
    {
        // At this stage, we have a memory to use so we can create the placer
        // This phase is the first one creating capable modules (workers) apart from myself
        if(properties & RL_BRING_UP) {
            phaseCount = policy->phasesPerRunlevel[RL_COMPUTE_OK][0] & 0xF;
            maxCount = policy->workerCount;
            for(i = rself->rlSwitch.nextPhase; i < phaseCount; ++i) {
                if(RL_IS_FIRST_PHASE_UP(policy, RL_COMPUTE_OK, i)) {
                    guidify(policy, (u64)policy, &(policy->fguid), OCR_GUID_POLICY);
                    // Create and initialize the placer (work in progress)
                    policy->placer = createLocationPlacer(policy);
                }
                toReturn |= helperSwitchInert(policy, runlevel, i, properties);

                // Setup the resume RL switch structure (in the synchronous case, used as
                // the counter we wait on)
                rself->rlSwitch.checkedIn = maxCount;
                rself->rlSwitch.runlevel = RL_COMPUTE_OK;
                rself->rlSwitch.nextPhase = i + 1;
                rself->rlSwitch.properties = origProperties;
                hal_fence();

                // Worker 0 is considered the capable one by convention
                // Still need to find out if the current PD is the "blessed" with mainEdt execution
                ocrGuid_t affinityMasterPD;
                u64 count = 0;
                // There should be a single master PD
                ASSERT(!ocrAffinityCount(AFFINITY_PD_MASTER, &count) && (count == 1));
                ocrAffinityGet(AFFINITY_PD_MASTER, &count, &affinityMasterPD);
                u16 blessed = ((policy->myLocation == affinityToLocation(affinityMasterPD)) ? RL_BLESSED : 0);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | blessed,
                    &hcWorkerCallback, RL_COMPUTE_OK << 16);

                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_COMPUTE_OK << 16) | j);
                }
                if(!fromPDMsg) {
                    // Here we need to block because when we return from the function, we need to have
                    // transitioned
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: synchronous switch to RL_COMPUTE_OK phase %d ... will block\n", i);
                    while(rself->rlSwitch.checkedIn) ;
                    ASSERT(rself->rlSwitch.checkedIn == 0);
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch to RL_COMPUTE_OK phase %d\n", i);
                    // We'll continue this from hcWorkerCallback
                    break; // Break out of the loop
                }
            }
        } else {
            // Tear down
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_COMPUTE_OK);
            // On bring down, we need to have at least two phases:
            //     - one where we actually stop the workers (asynchronously)
            //     - one where we join the workers (to clean up properly)
            ASSERT(phaseCount > 1);
            maxCount = policy->workerCount;

            // We do something special for the last phase in which we only have
            // one worker (all others should no longer be operating
            if(RL_IS_LAST_PHASE_DOWN(policy, RL_COMPUTE_OK, rself->rlSwitch.nextPhase)) {
                ASSERT(!fromPDMsg); // This last phase is done synchronously
                ASSERT(amPDMaster); // Only master worker should be here
                toReturn |= helperSwitchInert(policy, runlevel, rself->rlSwitch.nextPhase, properties);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, rself->rlSwitch.nextPhase,
                    properties | RL_PD_MASTER | RL_BLESSED, NULL, 0);
                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, rself->rlSwitch.nextPhase, properties, NULL, 0);
                }

                destroyLocationPlacer(policy);

                PD_MSG_STACK(msg);
                getCurrentEnv(NULL, NULL, NULL, &msg);
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
                msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD_I(guid) = policy->fguid;
                PD_MSG_FIELD_I(properties) = 0;
                toReturn |= policy->fcts.processMessage(policy, &msg, false);
                policy->fguid.guid = NULL_GUID;
#undef PD_MSG
#undef PD_TYPE
            } else { // Tear-down RL_USER_OK not last phase

                for(i = rself->rlSwitch.nextPhase; i > 0; --i) {
                    toReturn |= helperSwitchInert(policy, runlevel, i, properties);

                    // Setup the resume RL switch structure (in the synchronous case, used as
                    // the counter we wait on)
                    rself->rlSwitch.checkedIn = maxCount;
                    rself->rlSwitch.runlevel = RL_COMPUTE_OK;
                    rself->rlSwitch.nextPhase = i - 1;
                    rself->rlSwitch.properties = origProperties;
                    hal_fence();

                    // Worker 0 is considered the capable one by convention
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED,
                        &hcWorkerCallback, RL_COMPUTE_OK << 16);

                    for(j = 1; j < maxCount; ++j) {
                        toReturn |= policy->workers[j]->fcts.switchRunlevel(
                            policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_COMPUTE_OK << 16) | j);
                    }
                    if(!fromPDMsg) {
                        ASSERT(0); // Always from a PD message since it is from a shutdown message
                    } else {
                        DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch from RL_COMPUTE_OK phase %d\n", i);
                        // We'll continue this from hcWorkerCallback
                        break;
                    }
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_COMPUTE_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
        }
        break;
    }
    case RL_USER_OK:
    {
        if(properties & RL_BRING_UP) {
            // This branch is only taken in legacy mode the second time RL_USER_OK is entered
            if (rself->rlSwitch.legacySecondStart) {
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED, NULL, 0);
                // When I drop out of this, I should be in RL_COMPUTE_OK at phase 0
                // wait for everyone to check in so that I can continue shutting down
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker LEGACY mode, dropped out... waiting for others to complete RL\n");

                //Warning: here make sure the code in hcWorkerCallback reads all the rlSwitch
                //info BEFORE decrementing checkedIn. Otherwise there's a race on rlSwitch
                //between the following code and hcWorkerCallback.
                while(rself->rlSwitch.checkedIn != 0);

                ASSERT(rself->rlSwitch.runlevel == RL_COMPUTE_OK && rself->rlSwitch.nextPhase == 0);
                DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker LEGACY mode, wrapping up shutdown\n");
                // We complete the RL_COMPUTE_OK stage which will bring us down to RL_MEMORY_OK which will
                // get wrapped up by the outside code
                rself->rlSwitch.properties &= ~RL_FROM_MSG;
                toReturn |= policy->fcts.switchRunlevel(policy, rself->rlSwitch.runlevel,
                                                        rself->rlSwitch.properties | RL_PD_MASTER);
                return toReturn;
            }
            // BRING_UP is called twice in RL_LEGACY mode, record we've seen the first call.
            rself->rlSwitch.legacySecondStart = true;
            // Register properties here to allow tear down to read special flags set on bring up
            rself->rlSwitch.properties = properties;
            phaseCount = RL_GET_PHASE_COUNT_UP(policy, RL_USER_OK);
            maxCount = policy->workerCount;
            for(i = 0; i < phaseCount - 1; ++i) {
                if(toReturn) break;
                toReturn |= helperSwitchInert(policy, runlevel, i, properties);
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED, NULL, 0);
                for(j = 1; j < maxCount; ++j) {
                    // We start them in an async manner but don't need any callback (ie: we
                    // don't care if they have really started) since there is no bring-up barrier)
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
            }
            if(i == phaseCount - 1) { // Tests if we did not break out earlier with if(toReturn)
                toReturn |= helperSwitchInert(policy, runlevel, i, properties);
                for(j = 1; j < maxCount; ++j) {
                    // We start them in an async manner but don't need any callback (ie: we
                    // don't care if they have really started) since there is no bring-up barrier)
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, NULL, 0);
                }
                // Always do the capable worker last in this case (it will actualy start doing something useful)
                rself->rlSwitch.runlevel = RL_USER_OK;
                rself->rlSwitch.nextPhase = phaseCount;
                if (properties & RL_LEGACY) {
                    // In legacy mode the master worker just do its setup and falls-through
                    DPRINTF(DEBUG_LVL_VVERB, "Starting PD_MASTER worker in LEGACY mode\n");
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED, NULL, 0);
                } else {
                    toReturn |= policy->workers[0]->fcts.switchRunlevel(
                        policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED, NULL, 0);
                    // When I drop out of this, I should be in RL_COMPUTE_OK at phase 0
                    // wait for everyone to check in so that I can continue shutting down
                    DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker dropped out... waiting for others to complete RL\n");

                    //Warning: here make sure the code in hcWorkerCallback reads all the rlSwitch
                    //info BEFORE decrementing checkedIn. Otherwise there's a race on rlSwitch
                    //between the following code and hcWorkerCallback.
                    while(rself->rlSwitch.checkedIn != 0);

                    ASSERT(rself->rlSwitch.runlevel == RL_COMPUTE_OK && rself->rlSwitch.nextPhase == 0);
                    DPRINTF(DEBUG_LVL_VVERB, "PD_MASTER worker wrapping up shutdown\n");
                    // We complete the RL_COMPUTE_OK stage which will bring us down to RL_MEMORY_OK which will
                    // get wrapped up by the outside code
                    rself->rlSwitch.properties &= ~RL_FROM_MSG;
                    toReturn |= policy->fcts.switchRunlevel(policy, rself->rlSwitch.runlevel,
                                                            rself->rlSwitch.properties | RL_PD_MASTER);
                }
            }
        } else { // Tear down
            phaseCount = RL_GET_PHASE_COUNT_DOWN(policy, RL_USER_OK);
            maxCount = policy->workerCount;
            for(i = rself->rlSwitch.nextPhase; i >= 0; --i) {
                toReturn |= helperSwitchInert(policy, runlevel, i, properties);

                // Setup the resume RL switch structure (in the synchronous case, used as
                // the counter we wait on)
                rself->rlSwitch.checkedIn = maxCount;
                rself->rlSwitch.runlevel = RL_USER_OK;
                rself->rlSwitch.nextPhase = i - 1;
                rself->rlSwitch.properties = origProperties;
                hal_fence();

                // Worker 0 is considered the capable one by convention
                toReturn |= policy->workers[0]->fcts.switchRunlevel(
                    policy->workers[0], policy, runlevel, i, properties | RL_PD_MASTER | RL_BLESSED,
                    &hcWorkerCallback, RL_USER_OK << 16);

                for(j = 1; j < maxCount; ++j) {
                    toReturn |= policy->workers[j]->fcts.switchRunlevel(
                        policy->workers[j], policy, runlevel, i, properties, &hcWorkerCallback, (RL_USER_OK << 16) | j);
                }
                if(!fromPDMsg) {
                    ASSERT(0); // It should always be from a PD MSG since it is an asynchronous shutdown
                } else {
                    DPRINTF(DEBUG_LVL_VVERB, "switchRunlevel: asynchronous switch from RL_USER_OK phase %d\n", i);
                    // We'll continue this from hcWorkerCallback
                    break;
                }
            }
        }
        if(toReturn) {
            DPRINTF(DEBUG_LVL_WARN, "RL_USER_OK(%d) phase %d failed: %d\n", properties, i-1, toReturn);
        }
        break;
    }
    default:
        // Unknown runlevel
        ASSERT(0);
    }
    return toReturn;
}

void hcPolicyDomainDestruct(ocrPolicyDomain_t * policy) {
    // Destroying instances
    u64 i = 0;
    u64 maxCount = 0;
    //BUG #583: should transform all these to stop RL_DEALLOCATE

    // Note: As soon as worker '0' is stopped; its thread is
    // free to fall-through and continue shutting down the
    // policy domain

    maxCount = policy->workerCount;
    for(i = 0; i < maxCount; i++) {
        policy->workers[i]->fcts.destruct(policy->workers[i]);
    }

    maxCount = policy->commApiCount;
    for(i = 0; i < maxCount; i++) {
        policy->commApis[i]->fcts.destruct(policy->commApis[i]);
    }

    maxCount = policy->schedulerCount;
    for(i = 0; i < maxCount; ++i) {
        policy->schedulers[i]->fcts.destruct(policy->schedulers[i]);
    }

    // Destruct factories
    // Not all factories might have been used in the config file,
    // so only destroy them if they were instantiated.

    maxCount = policy->taskFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        if(policy->taskFactories[i])
            policy->taskFactories[i]->destruct(policy->taskFactories[i]);
    }

    maxCount = policy->eventFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        if(policy->eventFactories[i])
            policy->eventFactories[i]->destruct(policy->eventFactories[i]);
    }

    maxCount = policy->taskTemplateFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        if(policy->taskTemplateFactories[i])
            policy->taskTemplateFactories[i]->destruct(policy->taskTemplateFactories[i]);
    }

    maxCount = policy->dbFactoryCount;
    for(i = 0; i < maxCount; ++i) {
        if(policy->dbFactories[i])
            policy->dbFactories[i]->destruct(policy->dbFactories[i]);
    }

    //Anticipate those to be null-impl for some time
    ASSERT(policy->costFunction == NULL);

    // Destroy these last in case some of the other destructs make use of them
    maxCount = policy->guidProviderCount;
    for(i = 0; i < maxCount; ++i) {
        policy->guidProviders[i]->fcts.destruct(policy->guidProviders[i]);
    }

    maxCount = policy->allocatorCount;
    for(i = 0; i < maxCount; ++i) {
        policy->allocators[i]->fcts.destruct(policy->allocators[i]);
    }

    // Destroy self
    runtimeChunkFree((u64)policy->workers, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->commApis, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->schedulers, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->allocators, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->taskFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->taskTemplateFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->dbFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->eventFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->guidProviders, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy->schedulerObjectFactories, PERSISTENT_CHUNK);
    runtimeChunkFree((u64)policy, PERSISTENT_CHUNK);
}

static void localDeguidify(ocrPolicyDomain_t *self, ocrFatGuid_t *guid) {
    START_PROFILE(pd_hc_localDeguidify);
    ASSERT(self->guidProviderCount == 1);
    if(guid->guid != NULL_GUID && guid->guid != UNINITIALIZED_GUID) {
        if(guid->metaDataPtr == NULL) {
            self->guidProviders[0]->fcts.getVal(self->guidProviders[0], guid->guid,
                                                (u64*)(&(guid->metaDataPtr)), NULL);
        }
    }
    RETURN_PROFILE();
}

// In all these functions, we consider only a single PD. In other words, in CE, we
// deal with everything locally and never send messages

// allocateDatablock:  Utility used by hcAllocateDb and hcMemAlloc, just below.
static void* allocateDatablock (ocrPolicyDomain_t *self,
                                u64                size,
                                u64                prescription,
                                u64               *allocatorIdx) {
    void* result;
    u64 hints = 0; // Allocator hint
    u64 idx;  // Index into the allocators array to select the allocator to try.
    ASSERT (self->allocatorCount > 0);
    do {
        hints = (prescription & 1)?(OCR_ALLOC_HINT_NONE):(OCR_ALLOC_HINT_REDUCE_CONTENTION);
        prescription >>= 1;
        idx = prescription & 7;  // Get the index of the allocator to use.
        prescription >>= 3;
        if ((idx > self->allocatorCount) || (self->allocators[idx] == NULL)) {
            continue;  // Skip this allocator if it doesn't exist.
        }
        result = self->allocators[idx]->fcts.allocate(self->allocators[idx], size, hints);

        if (result) {
            *allocatorIdx = idx;
            return result;
        }
    } while (prescription != 0);
    return NULL;
}

static u8 hcAllocateDb(ocrPolicyDomain_t *self, ocrFatGuid_t *guid, void** ptr, u64 size,
                       u32 properties, ocrFatGuid_t affinity, ocrInDbAllocator_t allocator,
                       u64 prescription, ocrDataBlockType_t dbType) {
    // This function allocates a data block for the requestor, who is either this computing agent or a
    // different one that sent us a message.  After getting that data block, it "guidifies" the results
    // which, by the way, ultimately causes hcMemAlloc (just below) to run.
    //
    // Currently, the "affinity" and "allocator" arguments are ignored, and I expect that these will
    // eventually be eliminated here and instead, above this level, processed into the "prescription"
    // variable, which has been added to this argument list.  The prescription indicates an order in
    // which to attempt to allocate the block to a pool.
    u64 idx;
    void* result = allocateDatablock (self, size, prescription, &idx);
    if (result) {
        ocrDataBlock_t *block = self->dbFactories[0]->instantiate(
            self->dbFactories[0], self->allocators[idx]->fguid, self->fguid,
            size, result, properties, NULL);
        *ptr = result;
        (*guid).guid = block->guid;
        (*guid).metaDataPtr = block;

        // Notify scheduler of DB CREATE
        if (result != NULL && block != NULL) {
            ocrWorker_t *worker = NULL;
            PD_MSG_STACK(msg);
            getCurrentEnv(NULL, &worker, NULL, &msg);

#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_SCHED_NOTIFY
            msg.type = PD_MSG_SCHED_NOTIFY | PD_MSG_REQUEST;
            PD_MSG_FIELD_IO(schedArgs).kind = OCR_SCHED_NOTIFY_DB_CREATE;
            PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_DB_CREATE).guid.guid = block->guid;
            PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_DB_CREATE).guid.metaDataPtr = block;
            PD_MSG_FIELD_IO(schedArgs).OCR_SCHED_ARG_FIELD(OCR_SCHED_NOTIFY_DB_CREATE).dbType = dbType;
            RESULT_PROPAGATE(self->fcts.processMessage(self, &msg, false));
            ASSERT(PD_MSG_FIELD_O(returnDetail) != OCR_ENOTSUP);
#undef PD_MSG
#undef PD_TYPE
        }
        return 0;
    } else {
        DPRINTF(DEBUG_LVL_WARN, "hcAllocateDb returning NULL for size %ld\n", (u64) size);
        return OCR_ENOMEM;
    }
}

static u8 hcMemAlloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator, u64 size,
                     ocrMemType_t memType, void** ptr, u64 prescription) {
    // Like hcAllocateDb, this function also allocates a data block.  But it does NOT guidify
    // the results.  The main usage of this function is to allocate space for the guid needed
    // by hcAllocateDb; so if this function also guidified its results, you'd be in an infinite
    // guidification loop!
    //
    // The prescription indicates an order in which to attempt to allocate the block to a pool.
    void* result;
    u64 idx;
    ASSERT (memType == GUID_MEMTYPE || memType == DB_MEMTYPE);
    result = allocateDatablock (self, size, prescription, &idx);
    if (result) {
        *ptr = result;
        *allocator = self->allocators[idx]->fguid;
        return 0;
    } else {
        DPRINTF(DEBUG_LVL_WARN, "hcMemAlloc returning NULL for size %ld\n", (u64) size);
        return OCR_ENOMEM;
    }
}

static u8 hcMemUnAlloc(ocrPolicyDomain_t *self, ocrFatGuid_t* allocator,
                       void* ptr, ocrMemType_t memType) {
#if 1
    allocatorFreeFunction(ptr);
    return 0;
#else
    u64 i;
    ASSERT (memType == GUID_MEMTYPE || memType == DB_MEMTYPE);
    if (memType == DB_MEMTYPE) {
        for(i=0; i < self->allocatorCount; ++i) {
            if(self->allocators[i]->fguid.guid == allocator->guid) {
                allocator->metaDataPtr = self->allocators[i]->fguid.metaDataPtr;
                self->allocators[i]->fcts.free(self->allocators[i], ptr);
                return 0;
            }
        }
        return OCR_EINVAL;
    } else if (memType == GUID_MEMTYPE) {
        ASSERT (self->allocatorCount > 0);
        self->allocators[self->allocatorCount-1]->fcts.free(self->allocators[self->allocatorCount-1], ptr);
        return 0;
    } else {
        ASSERT (false);
    }
    return OCR_EINVAL;
#endif
}

/**
 * Checks validity of EDT create arguments and create an instance
 */
static u8 createEdtHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                      ocrFatGuid_t  edtTemplate, u32 *paramc, u64* paramv,
                      u32 *depc, u32 properties, ocrFatGuid_t affinity,
                      ocrFatGuid_t * outputEvent, ocrTask_t * currentEdt,
                      ocrFatGuid_t parentLatch) {
    ocrTaskTemplate_t *taskTemplate = (ocrTaskTemplate_t*)edtTemplate.metaDataPtr;
    DPRINTF(DEBUG_LVL_VVERB, "Creating EDT with template GUID 0x%lx (0x%lx) (paramc=%d; depc=%d)"
            " and have paramc=%d; depc=%d\n", edtTemplate.guid, edtTemplate.metaDataPtr,
            taskTemplate->paramc, taskTemplate->depc, *paramc, *depc);
    // Check that
    // 1. EDT doesn't have "default" as parameter count if the template
    //    was created with "unknown" as parameter count
    // 2. EDT has "default" as parameter count only if the template was created
    //    with a valid parameter count
    // 3. If neither of the above, the EDT & template both agree on the parameter count
    ASSERT(((taskTemplate->paramc == EDT_PARAM_UNK) && *paramc != EDT_PARAM_DEF) ||
           (taskTemplate->paramc != EDT_PARAM_UNK && (*paramc == EDT_PARAM_DEF ||
                   taskTemplate->paramc == *paramc)));
    // Check that
    // 1. EDT doesn't have "default" as dependence count if the template
    //    was created with "unknown" as dependence count
    // 2. EDT has "default" as dependence count only if the template was created
    //    with a valid dependence count
    // 3. If neither of the above, the EDT & template both agree on the dependence count
    ASSERT(((taskTemplate->depc == EDT_PARAM_UNK) && *depc != EDT_PARAM_DEF) ||
           (taskTemplate->depc != EDT_PARAM_UNK && (*depc == EDT_PARAM_DEF ||
                   taskTemplate->depc == *depc)));

    if(*paramc == EDT_PARAM_DEF) {
        *paramc = taskTemplate->paramc;
    }
    if(*depc == EDT_PARAM_DEF) {
        *depc = taskTemplate->depc;
    }

    // Check paramc/paramv combination validity
    if((*paramc > 0) && (paramv == NULL)) {
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to %d but paramv is NULL\n", *paramc);
        ASSERT(false);
        return OCR_EINVAL;
    }
    if((*paramc == 0) && (paramv != NULL)) {
        DPRINTF(DEBUG_LVL_WARN,"error: EDT paramc set to zero but paramv not NULL\n");
        ASSERT(false);
        return OCR_EINVAL;
    }
    u8 returnCode = self->taskFactories[0]->instantiate(
                           self->taskFactories[0], guid, edtTemplate, *paramc, paramv,
                           *depc, properties, affinity, outputEvent, currentEdt,
                           parentLatch, NULL);
    if(returnCode) {
        DPRINTF(DEBUG_LVL_WARN, "unable to create EDT, instantiate returnCode is %x\n", returnCode);
        ASSERT(false);
    }

    return returnCode;
}

static u8 createEdtTemplateHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                              ocrEdt_t func, u32 paramc, u32 depc, const char* funcName) {

    ocrTaskTemplate_t *base = self->taskTemplateFactories[0]->instantiate(
                                  self->taskTemplateFactories[0], func, paramc, depc, funcName, NULL);
    (*guid).guid = base->guid;
    (*guid).metaDataPtr = base;
    return 0;
}

static u8 createEventHelper(ocrPolicyDomain_t *self, ocrFatGuid_t *guid,
                        ocrEventTypes_t type, u32 properties) {

    return self->eventFactories[0]->instantiate(
        self->eventFactories[0], guid, type, properties, NULL);
}

static u8 convertDepAddToSatisfy(ocrPolicyDomain_t *self, ocrFatGuid_t dbGuid,
                                 ocrFatGuid_t destGuid, u32 slot) {

    PD_MSG_STACK(msg);
    ocrTask_t *curTask = NULL;
    getCurrentEnv(NULL, NULL, &curTask, &msg);
    ocrFatGuid_t currentEdt;
    currentEdt.guid = (curTask == NULL) ? NULL_GUID : curTask->guid;
    currentEdt.metaDataPtr = curTask;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_DEP_SATISFY
    msg.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
    PD_MSG_FIELD_I(satisfierGuid.guid) = curTask?curTask->guid:NULL_GUID;
    PD_MSG_FIELD_I(satisfierGuid.metaDataPtr) = curTask;
    PD_MSG_FIELD_I(guid) = destGuid;
    PD_MSG_FIELD_I(payload) = dbGuid;
    PD_MSG_FIELD_I(currentEdt) = currentEdt;
    PD_MSG_FIELD_I(slot) = slot;
    PD_MSG_FIELD_I(properties) = 0;
    RESULT_PROPAGATE(self->fcts.processMessage(self, &msg, false));
#undef PD_MSG
#undef PD_TYPE
    return 0;
}

#ifdef OCR_ENABLE_STATISTICS
static ocrStats_t* hcGetStats(ocrPolicyDomain_t *self) {
    return self->statsObject;
}
#endif

u8 hcPolicyDomainProcessMessage(ocrPolicyDomain_t *self, ocrPolicyMsg_t *msg, u8 isBlocking) {

    START_PROFILE(pd_hc_ProcessMessage);
    u8 returnCode = 0;

    // This assert checks the call's parameters are correct
    // - Synchronous processMessage calls always deal with a REQUEST.
    // - Asynchronous message processing allows for certain type of message
    //   to have a RESPONSE processed.
    ASSERT(((msg->type & PD_MSG_REQUEST) && !(msg->type & PD_MSG_RESPONSE))
        || ((msg->type & PD_MSG_RESPONSE) && ((msg->type & PD_MSG_TYPE_ONLY) == PD_MSG_DB_ACQUIRE)))

    // The message buffer size should always be greater or equal to the
    // max of the message in and out sizes, otherwise a write on the message
    // as a response overflows.
    u64 baseSizeIn = ocrPolicyMsgGetMsgBaseSize(msg, true);
    u64 baseSizeOut = ocrPolicyMsgGetMsgBaseSize(msg, false);
    ASSERT(((baseSizeIn < baseSizeOut) && (msg->bufferSize >= baseSizeOut)) || (baseSizeIn >= baseSizeOut));

    switch(msg->type & PD_MSG_TYPE_ONLY) {
    case PD_MSG_DB_CREATE: {
        START_PROFILE(pd_hc_DbCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_CREATE
        // BUG #584: Add properties whether DB needs to be acquired or not
        // This would impact where we do the PD_MSG_MEM_ALLOC for ex
        // For now we deal with both USER and RT dbs the same way
        ASSERT(PD_MSG_FIELD_I(dbType) == USER_DBTYPE || PD_MSG_FIELD_I(dbType) == RUNTIME_DBTYPE);
        ocrFatGuid_t tEdt = PD_MSG_FIELD_I(edt);
#define PRESCRIPTION 0x10LL
        PD_MSG_FIELD_O(returnDetail) = hcAllocateDb(self, &(PD_MSG_FIELD_IO(guid)),
                                  &(PD_MSG_FIELD_O(ptr)), PD_MSG_FIELD_IO(size),
                                  PD_MSG_FIELD_IO(properties),
                                  PD_MSG_FIELD_I(affinity),
                                  PD_MSG_FIELD_I(allocator),
                                  PRESCRIPTION, PD_MSG_FIELD_I(dbType));
        if(PD_MSG_FIELD_O(returnDetail) == 0) {
            ocrDataBlock_t *db = PD_MSG_FIELD_IO(guid.metaDataPtr);
            if(db==NULL){
                DPRINTF(DEBUG_LVL_WARN, "DB Create failed for size %lx\n", PD_MSG_FIELD_IO(size));
            }else{

                bool srcFlag;
                if(PD_MSG_FIELD_I(dbType) == USER_DBTYPE)
                    srcFlag = true;
                else
                    srcFlag = false;
                DPRINTF(DEBUG_LVL_VERB, "Creating a datablock of size %lu @ 0x%lx (GUID: 0x%lx)\n",
                        db->size, db->ptr, db->guid,
                        srcFlag, OCR_TRACE_TYPE_DATABLOCK, OCR_ACTION_CREATE, db->size);


            }
            ASSERT(db);
            // BUG #584: Check if properties want DB acquired
            ASSERT(db->fctId == self->dbFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.acquire(
                db, &(PD_MSG_FIELD_O(ptr)), tEdt, EDT_SLOT_NONE, DB_MODE_RW, !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE),
                (u32) DB_MODE_RW);
            // Set the default mode in the response message for the caller
            PD_MSG_FIELD_IO(properties) |= DB_MODE_RW;
        } else {
            // Cannot acquire
            PD_MSG_FIELD_O(ptr) = NULL;
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_DESTROY: {
        // Should never ever be called. The user calls free and internally
        // this will call whatever it needs (most likely PD_MSG_MEM_UNALLOC)
        // This would get called when DBs move for example
        ASSERT(0);
        break;
    }

    case PD_MSG_DB_ACQUIRE: {
        START_PROFILE(pd_hc_DbAcquire);
        if (msg->type & PD_MSG_REQUEST) {
        #define PD_MSG msg
        #define PD_TYPE PD_MSG_DB_ACQUIRE
            localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
            //BUG #273 rely on the call to set the fatguid ptr to NULL and not crash if edt acquiring is not local
            localDeguidify(self, &(PD_MSG_FIELD_IO(edt)));
            ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
            ASSERT(db->fctId == self->dbFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.acquire(
                db, &(PD_MSG_FIELD_O(ptr)), PD_MSG_FIELD_IO(edt), PD_MSG_FIELD_IO(edtSlot),
                (ocrDbAccessMode_t) (PD_MSG_FIELD_IO(properties) & (u32)DB_ACCESS_MODE_MASK),
                !!(PD_MSG_FIELD_IO(properties) & DB_PROP_RT_ACQUIRE), PD_MSG_FIELD_IO(properties));
            //BUG #273 db: modify the acquire call if we agree on changing the api
            PD_MSG_FIELD_O(size) = db->size;
            // conserve acquire's msg properties and add the DB's one.
            //BUG #273: This is related to bug #273
            PD_MSG_FIELD_IO(properties) |= db->flags;
            // Acquire message can be asynchronously responded to
            if (PD_MSG_FIELD_O(returnDetail) == OCR_EBUSY) {
                // Processing not completed
                returnCode = OCR_EPEND;
            } else {
                // Something went wrong in dbAcquire
                if(PD_MSG_FIELD_O(returnDetail)!=0)
                    DPRINTF(DEBUG_LVL_WARN, "DB Acquire failed for guid %lx\n", PD_MSG_FIELD_IO(guid));
                ASSERT(PD_MSG_FIELD_O(returnDetail) == 0);
                msg->type &= ~PD_MSG_REQUEST;
                msg->type |= PD_MSG_RESPONSE;
            }
        #undef PD_MSG
        #undef PD_TYPE
        } else {
            ASSERT(msg->type & PD_MSG_RESPONSE);
            // asynchronous callback on acquire, reading response
        #define PD_MSG msg
        #define PD_TYPE PD_MSG_DB_ACQUIRE
            ocrFatGuid_t edtFGuid = PD_MSG_FIELD_IO(edt);
            ocrFatGuid_t dbFGuid = PD_MSG_FIELD_IO(guid);
            u32 edtSlot = PD_MSG_FIELD_IO(edtSlot);
            ASSERT(edtSlot != EDT_SLOT_NONE); //BUG #190
            localDeguidify(self, &edtFGuid);
            // At this point the edt MUST be local as well as the acquire's message DB ptr
            ocrTask_t* task = (ocrTask_t*) edtFGuid.metaDataPtr;
            PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.dependenceResolved(task, dbFGuid.guid, PD_MSG_FIELD_O(ptr), edtSlot);
        #undef PD_MSG
        #undef PD_TYPE
        }
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_RELEASE: {
        // Call the appropriate release function
        START_PROFILE(pd_hc_DbRelease);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_RELEASE
        localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)));
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_IO(guid.metaDataPtr));
        ASSERT(db->fctId == self->dbFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.release(
            db, PD_MSG_FIELD_I(edt), !!(PD_MSG_FIELD_I(properties) & DB_PROP_RT_ACQUIRE));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DB_FREE: {
        // Call the appropriate free function
        START_PROFILE(pd_hc_DbFree);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DB_FREE
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        localDeguidify(self, &(PD_MSG_FIELD_I(edt)));
        ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        ASSERT(db->fctId == self->dbFactories[0]->factoryId);
        ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
        PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.free(
            db, PD_MSG_FIELD_I(edt), PD_MSG_FIELD_I(properties));
        if(PD_MSG_FIELD_O(returnDetail)!=0)
            DPRINTF(DEBUG_LVL_WARN, "DB Free failed for guid %lx\n", PD_MSG_FIELD_I(guid));
        else{
            DPRINTF(DEBUG_LVL_INFO,
                    "DB guid: %lx Destroyed\n", PD_MSG_FIELD_I(guid).guid,
                    false, OCR_TRACE_TYPE_DATABLOCK, OCR_ACTION_DESTROY);

        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
        // msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_ALLOC: {
        START_PROFILE(pd_hc_MemAlloc);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_ALLOC
        u64 tSize = PD_MSG_FIELD_I(size);
        ocrMemType_t tMemType = PD_MSG_FIELD_I(type);
        PD_MSG_FIELD_O(allocatingPD.metaDataPtr) = self;
        PD_MSG_FIELD_O(returnDetail) = hcMemAlloc(
            self, &(PD_MSG_FIELD_O(allocator)), tSize,
            tMemType, &(PD_MSG_FIELD_O(ptr)), PRESCRIPTION);
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_MEM_UNALLOC: {
        START_PROFILE(pd_hc_MemUnalloc);
#define PD_MSG msg
#define PD_TYPE PD_MSG_MEM_UNALLOC
        // This was set but it is in. Transforming into ASSERT but disabling for now
        // because I think we still have issues with allocator code
        //ASSERT(PD_MSG_FIELD_I(allocatingPD.metaDataPtr) == self);
        ocrFatGuid_t allocatorGuid = PD_MSG_FIELD_I(allocator);
        PD_MSG_FIELD_O(returnDetail) = hcMemUnAlloc(
            self, &allocatorGuid, PD_MSG_FIELD_I(ptr), PD_MSG_FIELD_I(type));
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_WORK_CREATE: {
        START_PROFILE(pd_hc_WorkCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
        localDeguidify(self, &(PD_MSG_FIELD_I(templateGuid)));
        if(PD_MSG_FIELD_I(templateGuid.metaDataPtr) == NULL)
            DPRINTF(DEBUG_LVL_WARN, "Invalid template GUID %lx\n", PD_MSG_FIELD_I(templateGuid));
        ASSERT(PD_MSG_FIELD_I(templateGuid.metaDataPtr) != NULL);
        localDeguidify(self, &(PD_MSG_FIELD_I(affinity)));
        localDeguidify(self, &(PD_MSG_FIELD_I(currentEdt)));
        localDeguidify(self, &(PD_MSG_FIELD_I(parentLatch)));

        ocrFatGuid_t *outputEvent = NULL;
        if(PD_MSG_FIELD_IO(outputEvent.guid) == UNINITIALIZED_GUID) {
            outputEvent = &(PD_MSG_FIELD_IO(outputEvent));
        }

        if((PD_MSG_FIELD_I(workType) != EDT_USER_WORKTYPE) && (PD_MSG_FIELD_I(workType) != EDT_RT_WORKTYPE)) {
            // This is a runtime error and should be reported as such
            DPRINTF(DEBUG_LVL_WARN, "Invalid worktype %x\n", PD_MSG_FIELD_I(workType));
        }

        ASSERT((PD_MSG_FIELD_I(workType) == EDT_USER_WORKTYPE) || (PD_MSG_FIELD_I(workType) == EDT_RT_WORKTYPE));
        u32 depc = PD_MSG_FIELD_IO(depc); // intentionally read before processing
        ocrFatGuid_t * depv = PD_MSG_FIELD_I(depv);
        u8 returnCode = createEdtHelper(
                self, &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(templateGuid),
                &(PD_MSG_FIELD_IO(paramc)), PD_MSG_FIELD_I(paramv), &(PD_MSG_FIELD_IO(depc)),
                PD_MSG_FIELD_I(properties), PD_MSG_FIELD_I(affinity), outputEvent,
                (ocrTask_t*)(PD_MSG_FIELD_I(currentEdt).metaDataPtr), PD_MSG_FIELD_I(parentLatch));
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            PD_MSG_FIELD_O(returnDetail) = returnCode;
        }
        ASSERT(returnCode == 0);
#ifndef EDT_DEPV_DELAYED
        if ((depv != NULL) && (returnCode == 0)) {
            ASSERT(depc != EDT_PARAM_DEF);
            ocrGuid_t destination = PD_MSG_FIELD_IO(guid).guid;
            u32 i = 0;
            ocrTask_t * curEdt = NULL;
            getCurrentEnv(NULL, NULL, &curEdt, NULL);
            ocrFatGuid_t curEdtFatGuid = {.guid = curEdt ? curEdt->guid : NULL_GUID, .metaDataPtr = curEdt};
            while(i < depc) {
                if(depv[i].guid != UNINITIALIZED_GUID) {
                    // We only add dependences that are not UNINITIALIZED_GUID
                    PD_MSG_STACK(msgAddDep);
                    getCurrentEnv(NULL, NULL, NULL, &msgAddDep);
                #undef PD_MSG
                #undef PD_TYPE
                    //NOTE: Could systematically call DEP_ADD but it's faster to disambiguate
                    //      NULL_GUID here instead of having DEP_ADD find out and do a satisfy.
                    if(depv[i].guid != NULL_GUID) {
                #define PD_MSG (&msgAddDep)
                #define PD_TYPE PD_MSG_DEP_ADD
                        msgAddDep.type = PD_MSG_DEP_ADD | PD_MSG_REQUEST;
                        PD_MSG_FIELD_I(source.guid) = depv[i].guid;
                        PD_MSG_FIELD_I(source.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(dest.guid) = destination;
                        PD_MSG_FIELD_I(dest.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(slot) = i;
                        PD_MSG_FIELD_IO(properties) = DB_DEFAULT_MODE;
                        PD_MSG_FIELD_I(currentEdt) = curEdtFatGuid;
                #undef PD_MSG
                #undef PD_TYPE
                    } else {
                      //Handle 'NULL_GUID' case here to avoid overhead of
                      //going through dep_add and end-up doing the same thing.
                #define PD_MSG (&msgAddDep)
                #define PD_TYPE PD_MSG_DEP_SATISFY
                        msgAddDep.type = PD_MSG_DEP_SATISFY | PD_MSG_REQUEST;
                        PD_MSG_FIELD_I(satisfierGuid.guid) = curEdtFatGuid.guid;
                        PD_MSG_FIELD_I(guid.guid) = destination;
                        PD_MSG_FIELD_I(guid.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(payload.guid) = NULL_GUID;
                        PD_MSG_FIELD_I(payload.metaDataPtr) = NULL;
                        PD_MSG_FIELD_I(slot) = i;
                        PD_MSG_FIELD_I(properties) = 0;
                        PD_MSG_FIELD_I(currentEdt) = curEdtFatGuid;
                #undef PD_MSG
                #undef PD_TYPE
                    }
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_CREATE
                    u8 toReturn = self->fcts.processMessage(self, &msgAddDep, true);
                    ASSERT(!toReturn);
                }
                ++i;
            }
        }
#endif
        // For asynchronous edt creation
        if (msg->type & PD_MSG_REQ_RESPONSE) {
            msg->type &= ~PD_MSG_REQUEST;
            msg->type |= PD_MSG_RESPONSE;
        }
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_WORK_EXECUTE: {
        ASSERT(0); // Not used for this PD
        break;
    }

    case PD_MSG_WORK_DESTROY: {
        START_PROFILE(pd_hc_WorkDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_WORK_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrTask_t *task = (ocrTask_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        if(task == NULL)
            DPRINTF(DEBUG_LVL_WARN, "Invalid task, guid %lx\n", PD_MSG_FIELD_I(guid));
        ASSERT(task);
        ASSERT(task->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.destruct(task);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_CREATE: {
        START_PROFILE(pd_hc_EdtTempCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_CREATE
#ifdef OCR_ENABLE_EDT_NAMING
            const char* edtName = PD_MSG_FIELD_I(funcName);
#else
            const char* edtName = "";
#endif
        PD_MSG_FIELD_O(returnDetail) = createEdtTemplateHelper(
            self, &(PD_MSG_FIELD_IO(guid)),
            PD_MSG_FIELD_I(funcPtr), PD_MSG_FIELD_I(paramc),
            PD_MSG_FIELD_I(depc), edtName);

        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EDTTEMP_DESTROY: {
        START_PROFILE(pd_hc_EdtTempDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EDTTEMP_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrTaskTemplate_t *tTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
        ASSERT(tTemplate->fctId == self->taskTemplateFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.destruct(tTemplate);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= (~PD_MSG_REQUEST);
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_CREATE: {
        START_PROFILE(pd_hc_EvtCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_CREATE
        PD_MSG_FIELD_O(returnDetail) = createEventHelper(
            self, &(PD_MSG_FIELD_IO(guid)),
            PD_MSG_FIELD_I(type), PD_MSG_FIELD_I(properties));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_EVT_DESTROY: {
        START_PROFILE(pd_hc_EvtDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrEvent_t *evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->fcts[evt->kind].destruct(evt);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= (~PD_MSG_REQUEST);
        EXIT_PROFILE;

        break;
    }

    case PD_MSG_EVT_GET: {
        START_PROFILE(pd_hc_EvtGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_EVT_GET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrEvent_t *evt = (ocrEvent_t*)PD_MSG_FIELD_I(guid.metaDataPtr);
        ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
        PD_MSG_FIELD_O(data) = self->eventFactories[0]->fcts[evt->kind].get(evt);
        // There's no way to check if this call has been
        // successful without changing the 'get' signature
        PD_MSG_FIELD_O(returnDetail) = 0;
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_CREATE: {
        START_PROFILE(pd_hc_GuidCreate);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_CREATE
        if(PD_MSG_FIELD_I(size) != 0) {
            // Here we need to create a metadata area as well
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.createGuid(
                self->guidProviders[0], &(PD_MSG_FIELD_IO(guid)), PD_MSG_FIELD_I(size),
                PD_MSG_FIELD_I(kind), PD_MSG_FIELD_I(properties));
        } else {
            // Here we just need to associate a GUID
            ocrGuid_t temp;
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getGuid(
                self->guidProviders[0], &temp, (u64)PD_MSG_FIELD_IO(guid.metaDataPtr),
                PD_MSG_FIELD_I(kind));
            PD_MSG_FIELD_IO(guid.guid) = temp;
        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_INFO: {
        START_PROFILE(pd_hc_GuidInfo);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_INFO
        localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
        if(PD_MSG_FIELD_I(properties) & KIND_GUIDPROP) {
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getKind(
                self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(kind)));
            if(PD_MSG_FIELD_O(returnDetail) == 0)
                PD_MSG_FIELD_O(returnDetail) = KIND_GUIDPROP
                    | WMETA_GUIDPROP | RMETA_GUIDPROP;
        } else if (PD_MSG_FIELD_I(properties) & LOCATION_GUIDPROP) {
            PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.getLocation(
                self->guidProviders[0], PD_MSG_FIELD_IO(guid.guid), &(PD_MSG_FIELD_O(location)));
            if(PD_MSG_FIELD_O(returnDetail) == 0)
                PD_MSG_FIELD_O(returnDetail) = LOCATION_GUIDPROP
                    | WMETA_GUIDPROP | RMETA_GUIDPROP;
        } else {
            PD_MSG_FIELD_O(returnDetail) = WMETA_GUIDPROP | RMETA_GUIDPROP;
        }

#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_GUID_METADATA_CLONE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_METADATA_CLONE
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_IO(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        //IMPL: For now only support edt template cloning

        switch(kind) {
            case OCR_GUID_EDT_TEMPLATE:
                localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                //These won't support flat serialization
#ifdef OCR_ENABLE_STATISTICS
                ASSERT(false && "no statistics support in distributed edt templates");
#endif
                PD_MSG_FIELD_O(size) = sizeof(ocrTaskTemplateHc_t) + (sizeof(u64) * OCR_HINT_COUNT_EDT_HC);
                PD_MSG_FIELD_O(returnDetail) = 0;
                break;
            case OCR_GUID_AFFINITY:
                localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                PD_MSG_FIELD_O(size) = sizeof(ocrAffinity_t);
                PD_MSG_FIELD_O(returnDetail) = 0;
                break;
#ifdef ENABLE_EXTENSION_LABELING
            case OCR_GUID_GUIDMAP:
                localDeguidify(self, &(PD_MSG_FIELD_IO(guid)));
                ocrGuidMap_t * map = (ocrGuidMap_t *) PD_MSG_FIELD_IO(guid.metaDataPtr);
                PD_MSG_FIELD_O(size) =  ((sizeof(ocrGuidMap_t) + sizeof(s64) - 1) & ~(sizeof(s64)-1)) + map->numParams*sizeof(s64);
                PD_MSG_FIELD_O(returnDetail) = 0;
                break;
#endif
            default:
                ASSERT(false && "Unsupported GUID kind cloning");
                PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }
    case PD_MSG_GUID_RESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_RESERVE
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidReserve(
            self->guidProviders[0], &(PD_MSG_FIELD_O(startGuid)), &(PD_MSG_FIELD_O(skipGuid)),
            PD_MSG_FIELD_I(numberGuids), PD_MSG_FIELD_I(guidKind));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_GUID_UNRESERVE:
    {
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_RESERVE
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.guidUnreserve(
            self->guidProviders[0], PD_MSG_FIELD_O(startGuid), PD_MSG_FIELD_O(skipGuid),
            PD_MSG_FIELD_I(numberGuids));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }
    case PD_MSG_GUID_DESTROY: {
        START_PROFILE(pd_hc_GuidDestroy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_GUID_DESTROY
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        PD_MSG_FIELD_O(returnDetail) = self->guidProviders[0]->fcts.releaseGuid(
            self->guidProviders[0], PD_MSG_FIELD_I(guid), PD_MSG_FIELD_I(properties) & 1);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_GET_WORK: {
        START_PROFILE(pd_hc_Sched_Work);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_GET_WORK
        ocrSchedulerOpWorkArgs_t *taskArgs = &PD_MSG_FIELD_IO(schedArgs);
        taskArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_GET_WORK].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)taskArgs, NULL);

        if (taskArgs->kind == OCR_SCHED_WORK_EDT_USER) {
            PD_MSG_FIELD_O(factoryId) = 0; //taskHc_id;
            ocrFatGuid_t *fguid = &(taskArgs->OCR_SCHED_ARG_FIELD(OCR_SCHED_WORK_EDT_USER).edt);
            localDeguidify(self, fguid);
        }
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_NOTIFY: {
        START_PROFILE(pd_hc_Sched_Notify);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_NOTIFY
        ocrSchedulerOpNotifyArgs_t *notifyArgs = &PD_MSG_FIELD_IO(schedArgs);
        notifyArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_NOTIFY].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)notifyArgs, NULL);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SCHED_ANALYZE: {
        START_PROFILE(pd_hc_Sched_Notify);
#define PD_MSG msg
#define PD_TYPE PD_MSG_SCHED_ANALYZE
        ocrSchedulerOpAnalyzeArgs_t *analyzeArgs = &PD_MSG_FIELD_IO(schedArgs);
        analyzeArgs->base.location = msg->srcLocation;
        PD_MSG_FIELD_O(returnDetail) =
            self->schedulers[0]->fcts.op[OCR_SCHEDULER_OP_ANALYZE].invoke(
                self->schedulers[0], (ocrSchedulerOpArgs_t*)analyzeArgs, NULL);
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_ADD: {
        START_PROFILE(pd_hc_AddDep);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_ADD
        // We first get information about the source and destination
        ocrGuidKind srcKind, dstKind;
        //NOTE: In distributed the metaDataPtr is set to NULL_GUID since
        //the guid provider doesn't fetch remote metaDataPtr yet. It's ok
        //(but fragile) because the HC event/task does not try to use it
        //Querying the kind through the PD's interface should be ok as it's
        //the problem of the guid provider to give this information
        if (PD_MSG_FIELD_I(source.guid) == NULL_GUID) {
            srcKind = OCR_GUID_NONE;
        } else {
            self->guidProviders[0]->fcts.getVal(
                self->guidProviders[0], PD_MSG_FIELD_I(source.guid),
                (u64*)(&(PD_MSG_FIELD_I(source.metaDataPtr))), &srcKind);
        }
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
            (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind);

        ocrFatGuid_t src = PD_MSG_FIELD_I(source);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        ocrDbAccessMode_t mode = (PD_MSG_FIELD_IO(properties) & DB_ACCESS_MODE_MASK); //lower bits is the mode //BUG 550: not pretty
        u32 slot = PD_MSG_FIELD_I(slot);

        if (srcKind == OCR_GUID_NONE) {
            //NOTE: Handle 'NULL_GUID' case here to be safe although
            //we've already caught it in ocrAddDependence for performance
            // This is equivalent to an immediate satisfy
            PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                self, src, dest, slot);
        } else if (srcKind == OCR_GUID_DB) {
            if (dstKind & OCR_GUID_EVENT) {
                PD_MSG_FIELD_O(returnDetail) = convertDepAddToSatisfy(
                    self, src, dest, slot);
            } else {
                // NOTE: We could use convertDepAddToSatisfy since adding a DB dependence
                // is equivalent to satisfy. However, we want to go through the register
                // function to make sure the access mode is recorded.
                if(dstKind != OCR_GUID_EDT)
                    DPRINTF(DEBUG_LVL_WARN, "Attempting to add a DB dependence to dest of kind %x "
                                            "that's neither EDT nor Event\n", dstKind);
                ASSERT(dstKind == OCR_GUID_EDT);
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGSIGNALER
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = src;
                PD_MSG_FIELD_I(dest) = dest;
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;
            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
            }
        } else {
            if(!(srcKind & OCR_GUID_EVENT)) {
                DPRINTF(DEBUG_LVL_WARN, "Attempting to add a dependence with a GUID of type 0x%x, "
                                        "expected Event\n", srcKind);
            }
            ASSERT(srcKind & OCR_GUID_EVENT);
            // We are handling the following dependences (event, event|edt) and do
            // things differently depending on the type of events:
            // (non-persistent event, edt)  => REG_SIGNALER (event on edt), then REG_WAITER (edt on event)
            // (persistent event, edt)      => REG_SIGNALER (event on edt, edt does late registration)
            // ((any) event, (any) event)   => REG_WAITER
            //
            // Are we revealing too much of the underlying implementation here ?
            //
            bool srcIsNonPersistent = ((srcKind == OCR_GUID_EVENT_ONCE) ||
                                        (srcKind == OCR_GUID_EVENT_LATCH));
            // 'Push' registration when source is non-persistent and/or destination is another event.
            bool needPushMode = (srcIsNonPersistent || (dstKind & OCR_GUID_EVENT));
            // The registration is always necessary when the destination is an EDT.
            // It allows to record the mode of the dependence as well as the type of
            // event the EDT should be expecting.
            bool needPullMode = !!(dstKind & OCR_GUID_EDT);
            // NOTE: Important to do the signaler registration before the waiter one
            // when the dependence is of the form (non-persistent event, edt)
            // Otherwise there's a race between the once event being destroyed and
            // the edt processing the registerSignaler call (which may read into the
            // destroyed event metadata).
            if(needPullMode) {
                ASSERT_BLOCK_BEGIN(dstKind & OCR_GUID_EDT);
                DPRINTF(DEBUG_LVL_WARN, "Runtime error expect REGSIGNALER dest to be an EDT GUID\n");
                ASSERT_BLOCK_END
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
                // 'Pull' registration left with persistent event as source and EDT as destination
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGSIGNALER
                registerMsg.type = PD_MSG_DEP_REGSIGNALER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                // Registers sourceGuid (signaler) onto destGuid
                PD_MSG_FIELD_I(signaler) = src;
                PD_MSG_FIELD_I(dest) = dest;
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(mode) = mode;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;

                DPRINTF(DEBUG_LVL_INFO,
                        "Dependence added (src: 0x%lx, dest: 0x%lx) -> %u\n", src.guid, dest.guid, returnCode,
                        false, OCR_TRACE_TYPE_EDT, OCR_ACTION_ADD_DEP, src.guid);

            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
            }

            if (needPushMode) {
                //OK if srcKind is at current location
                PD_MSG_STACK(registerMsg);
                getCurrentEnv(NULL, NULL, NULL, &registerMsg);
            #define PD_MSG (&registerMsg)
            #define PD_TYPE PD_MSG_DEP_REGWAITER
                // Registration with non-persistent events is two-way
                // to enforce message ordering constraints.
                registerMsg.type = PD_MSG_DEP_REGWAITER | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
                // Registers destGuid (waiter) onto sourceGuid
                PD_MSG_FIELD_I(waiter) = dest;
                PD_MSG_FIELD_I(dest) = src;
                PD_MSG_FIELD_I(slot) = slot;
                PD_MSG_FIELD_I(properties) = true; // Specify context is add-dependence
                u8 returnCode = self->fcts.processMessage(self, &registerMsg, true);
                u8 returnDetail = (returnCode == 0) ? PD_MSG_FIELD_O(returnDetail) : returnCode;

                DPRINTF(DEBUG_LVL_INFO,
                        "Dependence added (src: 0x%lx, dest: 0x%lx) -> %u\n", src.guid, dest.guid, returnCode,
                        false, OCR_TRACE_TYPE_EVENT, OCR_ACTION_ADD_DEP, src.guid);

            #undef PD_MSG
            #undef PD_TYPE
            #define PD_MSG msg
            #define PD_TYPE PD_MSG_DEP_ADD
                PD_MSG_FIELD_O(returnDetail) = returnDetail;
                RESULT_PROPAGATE(returnCode);
            #undef PD_MSG
            #undef PD_TYPE
            }
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGSIGNALER: {
        START_PROFILE(pd_hc_RegSignaler);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGSIGNALER
        // We first get information about the signaler and destination
        ocrGuidKind signalerKind, dstKind;
        //NOTE: In distributed the metaDataPtr is set to NULL_GUID since
        //the guid provider doesn't fetch remote metaDataPtr yet. It's ok
        //(but fragile) because the HC event/task does not try to use it
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(signaler.guid),
            (u64*)(&(PD_MSG_FIELD_I(signaler.metaDataPtr))), &signalerKind);
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
            (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind);

        ocrFatGuid_t signaler = PD_MSG_FIELD_I(signaler);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        bool isAddDep = PD_MSG_FIELD_I(properties);

        if (dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
            ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->fcts[evt->kind].registerSignaler(
                evt, signaler, PD_MSG_FIELD_I(slot), PD_MSG_FIELD_I(mode), isAddDep);
        } else if (dstKind == OCR_GUID_EDT) {
            ocrTask_t *edt = (ocrTask_t*)(dest.metaDataPtr);
            ASSERT(edt->fctId == self->taskFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.registerSignaler(
                edt, signaler, PD_MSG_FIELD_I(slot), PD_MSG_FIELD_I(mode), isAddDep);
        } else {
            DPRINTF(DEBUG_LVL_WARN, "Attempt to register signaler on %x which is not of type EDT or Event\n", dstKind);
            ASSERT(0); // No other things we can register signalers on
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif

        DPRINTF(DEBUG_LVL_INFO,
                "Dependence added (src: 0x%lx, dest: 0x%lx) -> %u\n", signaler.guid, dest.guid, returnCode,
                false, OCR_TRACE_TYPE_EDT, OCR_ACTION_ADD_DEP, signaler.guid);


#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_REGWAITER: {
        START_PROFILE(pd_hc_RegWaiter);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_REGWAITER
        // We first get information about the signaler and destination
        ocrGuidKind dstKind;
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(dest.guid),
            (u64*)(&(PD_MSG_FIELD_I(dest.metaDataPtr))), &dstKind);
        ocrFatGuid_t waiter = PD_MSG_FIELD_I(waiter);
        ocrFatGuid_t dest = PD_MSG_FIELD_I(dest);
        bool isAddDep = PD_MSG_FIELD_I(properties);
        if (dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dest.metaDataPtr);
            ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->fcts[evt->kind].registerWaiter(
                evt, waiter, PD_MSG_FIELD_I(slot), isAddDep);
        } else {
            if((dstKind & OCR_GUID_DB) == 0)
                DPRINTF(DEBUG_LVL_WARN, "Attempting to add a dependence to a GUID of type %x, expected DB\n", dstKind);
            ASSERT(dstKind & OCR_GUID_DB);
            // When an EDT want to register to a DB, for instance to get EW access.
            ocrDataBlock_t *db = (ocrDataBlock_t*)(dest.metaDataPtr);
            ASSERT(db->fctId == self->dbFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.registerWaiter(
                db, waiter, PD_MSG_FIELD_I(slot), isAddDep);
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }


    case PD_MSG_DEP_SATISFY: {
        START_PROFILE(pd_hc_Satisfy);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_SATISFY
        // make sure this is one-way
        ASSERT(!(msg->type & PD_MSG_REQ_RESPONSE));
        ocrGuidKind dstKind;
        ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t *)self;
        self->guidProviders[0]->fcts.getVal(
            self->guidProviders[0], PD_MSG_FIELD_I(guid.guid),
            (u64*)(&(PD_MSG_FIELD_I(guid.metaDataPtr))), &dstKind);

        ocrFatGuid_t dst = PD_MSG_FIELD_I(guid);
        if(dstKind & OCR_GUID_EVENT) {
            ocrEvent_t *evt = (ocrEvent_t*)(dst.metaDataPtr);
            ASSERT(evt->fctId == self->eventFactories[0]->factoryId);
            PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->fcts[evt->kind].satisfy(
                evt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
            rself->pqrFlags.prevDb = PD_MSG_FIELD_I(payload).guid;
        } else {
            if(dstKind == OCR_GUID_EDT) {
                ocrTask_t *edt = (ocrTask_t*)(dst.metaDataPtr);
                ASSERT(edt->fctId == self->taskFactories[0]->factoryId);
                PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.satisfy(
                    edt, PD_MSG_FIELD_I(payload), PD_MSG_FIELD_I(slot));
                rself->pqrFlags.prevDb = PD_MSG_FIELD_I(payload).guid;
            } else {
                DPRINTF(DEBUG_LVL_WARN, "Attempting to satisfy a GUID of type %x, expected EDT\n", dstKind);
                PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
                ASSERT(0); // We can't satisfy anything else
            }
        }
#ifdef OCR_ENABLE_STATISTICS
        statsDEP_ADD(pd, getCurrentEDT(), NULL, signalerGuid, waiterGuid, NULL, slot);
#endif
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_UNREGSIGNALER: {
        //Not implemented: see #521, #522
        ASSERT(0);
        break;
    }

    case PD_MSG_DEP_UNREGWAITER: {
        //Not implemented: see #521, #522
        ASSERT(0);
        break;
    }

    case PD_MSG_DEP_DYNADD: {
        START_PROFILE(pd_hc_DynAdd);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNADD
        // Check to make sure that the EDT is only doing this to
        // itself
        // Also, this should only happen when there is an actual EDT
        if((curTask==NULL) || (curTask->guid != PD_MSG_FIELD_I(edt.guid)))
            DPRINTF(DEBUG_LVL_WARN, "Attempting to notify a missing/different EDT, GUID=%lx\n", PD_MSG_FIELD_I(edt.guid));
        ASSERT(curTask &&
               curTask->guid == PD_MSG_FIELD_I(edt.guid));

        ASSERT(curTask->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.notifyDbAcquire(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        // msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_DEP_DYNREMOVE: {
        START_PROFILE(pd_hc_DynRemove);
        ocrTask_t *curTask = NULL;
        getCurrentEnv(NULL, NULL, &curTask, NULL);
#define PD_MSG msg
#define PD_TYPE PD_MSG_DEP_DYNREMOVE
        // Check to make sure that the EDT is only doing this to itself
        // Also, this should only happen when there is an actual EDT
        if ((curTask==NULL) || (curTask->guid != PD_MSG_FIELD_I(edt.guid)))
            DPRINTF(DEBUG_LVL_WARN, "Attempting to notify a missing/different EDT, GUID=%lx\n", PD_MSG_FIELD_I(edt.guid));
        ASSERT(curTask && curTask->guid == PD_MSG_FIELD_I(edt.guid));
        ASSERT(curTask->fctId == self->taskFactories[0]->factoryId);
        PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.notifyDbRelease(curTask, PD_MSG_FIELD_I(db));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_SAL_PRINT: {
        ASSERT(0);
        break;
    }

    case PD_MSG_SAL_READ: {
        ASSERT(0);
        break;
    }

    case PD_MSG_SAL_WRITE: {
        ASSERT(0);
        break;
    }

    case PD_MSG_SAL_TERMINATE: {
        ASSERT(0);
        break;
    }

    case PD_MSG_MGT_REGISTER: {
        ASSERT(0);
        break;
    }

    case PD_MSG_MGT_UNREGISTER: {
        // Only one PD at this time
        ASSERT(0);
        break;
    }

    case PD_MSG_MGT_RL_NOTIFY: {
#define PD_MSG msg
#define PD_TYPE PD_MSG_MGT_RL_NOTIFY
        if(PD_MSG_FIELD_I(properties) & RL_FROM_MSG) {
            // This should not happen here as we only have one PD
            ASSERT(0);
        } else {
            // This is from user code so it should be a request to shutdown
            ocrPolicyDomainHc_t *rself = (ocrPolicyDomainHc_t*)self;
            // Set up the switching for the next phase
            ASSERT(rself->rlSwitch.runlevel == RL_USER_OK && rself->rlSwitch.nextPhase == RL_GET_PHASE_COUNT_UP(self, RL_USER_OK));
            // We want to transition down now
            rself->rlSwitch.nextPhase = RL_GET_PHASE_COUNT_DOWN(self, RL_USER_OK) - 1;
            ASSERT(PD_MSG_FIELD_I(properties) & RL_TEAR_DOWN);
            ASSERT(PD_MSG_FIELD_I(runlevel) & RL_COMPUTE_OK);
            self->shutdownCode = PD_MSG_FIELD_I(errorCode);
            u8 returnCode = self->fcts.switchRunlevel(
                              self, RL_USER_OK, RL_TEAR_DOWN | RL_ASYNC | RL_REQUEST | RL_FROM_MSG);
            ASSERT(returnCode == 0);
        }
        msg->type &= ~PD_MSG_REQUEST;
        break;
#undef PD_MSG
#undef PD_TYPE
    }

    case PD_MSG_MGT_MONITOR_PROGRESS: {
#define PD_MSG (msg)
#define PD_TYPE PD_MSG_MGT_MONITOR_PROGRESS
        // Delegate to scheduler
        PD_MSG_FIELD_IO(properties) = self->schedulers[0]->fcts.monitorProgress(self->schedulers[0],
                                                                                (ocrMonitorProgress_t) PD_MSG_FIELD_IO(properties) & 0xFF, PD_MSG_FIELD_I(monitoree));
#undef PD_MSG
#undef PD_TYPE
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
        break;
    }

    case PD_MSG_HINT_SET: {
        START_PROFILE(pd_hc_HintSet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_SET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_I(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        switch(PD_MSG_FIELD_I(hint.type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.setHint(taskTemplate, &(PD_MSG_FIELD_I(hint)));
                } else {
                    ASSERT(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.setHint(task, &(PD_MSG_FIELD_I(hint)));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ASSERT(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.setHint(db, &(PD_MSG_FIELD_I(hint)));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ASSERT(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->commonFcts.setHint(evt, &(PD_MSG_FIELD_I(hint)));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ASSERT(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    case PD_MSG_HINT_GET: {
        START_PROFILE(pd_hc_HintGet);
#define PD_MSG msg
#define PD_TYPE PD_MSG_HINT_GET
        localDeguidify(self, &(PD_MSG_FIELD_I(guid)));
        ocrFatGuid_t fatGuid = PD_MSG_FIELD_I(guid);
        ocrGuidKind kind = OCR_GUID_NONE;
        guidKind(self, fatGuid, &kind);
        switch(PD_MSG_FIELD_IO(hint.type)) {
        case OCR_HINT_EDT_T:
            {
                if (kind == OCR_GUID_EDT_TEMPLATE) {
                    ocrTaskTemplate_t* taskTemplate = (ocrTaskTemplate_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskTemplateFactories[0]->fcts.getHint(taskTemplate, &(PD_MSG_FIELD_IO(hint)));
                } else {
                    ASSERT(kind == OCR_GUID_EDT);
                    ocrTask_t *task = (ocrTask_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                    PD_MSG_FIELD_O(returnDetail) = self->taskFactories[0]->fcts.getHint(task, &(PD_MSG_FIELD_IO(hint)));
                }
            }
            break;
        case OCR_HINT_DB_T:
            {
                ASSERT(kind == OCR_GUID_DB);
                ocrDataBlock_t *db = (ocrDataBlock_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->dbFactories[0]->fcts.getHint(db, &(PD_MSG_FIELD_IO(hint)));
            }
            break;
        case OCR_HINT_EVT_T:
            {
                ASSERT(kind & OCR_GUID_EVENT);
                ocrEvent_t *evt = (ocrEvent_t*)(PD_MSG_FIELD_I(guid.metaDataPtr));
                PD_MSG_FIELD_O(returnDetail) = self->eventFactories[0]->commonFcts.getHint(evt, &(PD_MSG_FIELD_IO(hint)));
            }
            break;
        case OCR_HINT_GROUP_T:
        default:
            ASSERT(0);
            PD_MSG_FIELD_O(returnDetail) = OCR_ENOTSUP;
            break;
        }
        msg->type &= ~PD_MSG_REQUEST;
        msg->type |= PD_MSG_RESPONSE;
#undef PD_MSG
#undef PD_TYPE
        EXIT_PROFILE;
        break;
    }

    default:
        // Not handled
        ASSERT(0);
    }

    if ((msg->type & PD_MSG_RESPONSE) && (msg->type & PD_MSG_REQ_RESPONSE)) {
        // response is issued:
        // flip required response bit
        msg->type &= ~PD_MSG_REQ_RESPONSE;
        // flip src and dest locations
        ocrLocation_t src = msg->srcLocation;
        msg->srcLocation = msg->destLocation;
        msg->destLocation = src;
    } // when (!PD_MSG_REQ_RESPONSE) we were processing an asynchronous processMessage's RESPONSE

    // This code is not needed but just shows how things would be handled (probably
    // done by sub-functions)
    if(isBlocking && (msg->type & PD_MSG_REQ_RESPONSE)) {
        ASSERT(msg->type & PD_MSG_RESPONSE); // If we were blocking and needed a response
        // we need to make sure there is one
    }

    RETURN_PROFILE(returnCode);
}

u8 hcPdSendMessage(ocrPolicyDomain_t* self, ocrLocation_t target, ocrPolicyMsg_t *message,
                   ocrMsgHandle_t **handle, u32 properties) {
    ASSERT(0);
    return 0;
}

u8 hcPdPollMessage(ocrPolicyDomain_t *self, ocrMsgHandle_t **handle) {
    ASSERT(0);
    return 0;
}

u8 hcPdWaitMessage(ocrPolicyDomain_t *self,  ocrMsgHandle_t **handle) {
    ASSERT(0);
    return 0;
}

void* hcPdMalloc(ocrPolicyDomain_t *self, u64 size) {
    START_PROFILE(pd_hc_PdMalloc);
    // Just try in the first allocator
    void* toReturn = NULL;
    toReturn = self->allocators[0]->fcts.allocate(self->allocators[0], size, 0);
    if(toReturn == NULL)
        DPRINTF(DEBUG_LVL_WARN, "Failed PDMalloc for size %lx\n", size);
    ASSERT(toReturn != NULL);
    RETURN_PROFILE(toReturn);
}

void hcPdFree(ocrPolicyDomain_t *self, void* addr) {
    START_PROFILE(pd_hc_PdFree);
    // May result in leaks but better than the alternative...

    allocatorFreeFunction(addr);

    RETURN_PROFILE();
}

ocrPolicyDomain_t * newPolicyDomainHc(ocrPolicyDomainFactory_t * factory,
#ifdef OCR_ENABLE_STATISTICS
                                      ocrStats_t *statsObject,
#endif
                                      ocrCost_t *costFunction, ocrParamList_t *perInstance) {

    ocrPolicyDomainHc_t * derived = (ocrPolicyDomainHc_t *) runtimeChunkAlloc(sizeof(ocrPolicyDomainHc_t), PERSISTENT_CHUNK);
    ocrPolicyDomain_t * base = (ocrPolicyDomain_t *) derived;
    ASSERT(base);
#ifdef OCR_ENABLE_STATISTICS
    factory->initialize(factory, base, statsObject, costFunction, perInstance);
#else
    factory->initialize(factory, base, costFunction, perInstance);
#endif

    return base;
}

void initializePolicyDomainHc(ocrPolicyDomainFactory_t * factory, ocrPolicyDomain_t* self,
#ifdef OCR_ENABLE_STATISTICS
                              ocrStats_t *statsObject,
#endif
                              ocrCost_t *costFunction, ocrParamList_t *perInstance) {
#ifdef OCR_ENABLE_STATISTICS
    self->statsObject = statsObject;
#endif

    initializePolicyDomainOcr(factory, self, perInstance);

    ocrPolicyDomainHc_t* derived = (ocrPolicyDomainHc_t*) self;
    derived->rlSwitch.legacySecondStart = false;
}

static void destructPolicyDomainFactoryHc(ocrPolicyDomainFactory_t * factory) {
    runtimeChunkFree((u64)factory, NONPERSISTENT_CHUNK);
}

ocrPolicyDomainFactory_t * newPolicyDomainFactoryHc(ocrParamList_t *perType) {
    ocrPolicyDomainFactory_t* base = (ocrPolicyDomainFactory_t*) runtimeChunkAlloc(sizeof(ocrPolicyDomainFactoryHc_t), NONPERSISTENT_CHUNK);
    // Set factory's methods
#ifdef OCR_ENABLE_STATISTICS
    base->instantiate = FUNC_ADDR(ocrPolicyDomain_t*(*)(ocrPolicyDomainFactory_t*,ocrStats_t*,
                                  ocrCost_t *,ocrParamList_t*), newPolicyDomainHc);
    base->initialize = FUNC_ADDR(void(*)(ocrPolicyDomainFactory_t*,ocrPolicyDomain_t*,
                                         ocrStats_t*,ocrCost_t *,ocrParamList_t*), initializePolicyDomainHc);
#endif

    base->instantiate = &newPolicyDomainHc;
    base->initialize = &initializePolicyDomainHc;
    base->destruct = &destructPolicyDomainFactoryHc;

    // Set future PDs' instance  methods
    base->policyDomainFcts.destruct = FUNC_ADDR(void(*)(ocrPolicyDomain_t*), hcPolicyDomainDestruct);
    base->policyDomainFcts.switchRunlevel = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrRunlevel_t, u32), hcPdSwitchRunlevel);
    base->policyDomainFcts.processMessage = FUNC_ADDR(u8(*)(ocrPolicyDomain_t*,ocrPolicyMsg_t*,u8), hcPolicyDomainProcessMessage);

    base->policyDomainFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrLocation_t, ocrPolicyMsg_t *, ocrMsgHandle_t**, u32),
                                         hcPdSendMessage);
    base->policyDomainFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcPdPollMessage);
    base->policyDomainFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrPolicyDomain_t*, ocrMsgHandle_t**), hcPdWaitMessage);

    base->policyDomainFcts.pdMalloc = FUNC_ADDR(void*(*)(ocrPolicyDomain_t*,u64), hcPdMalloc);
    base->policyDomainFcts.pdFree = FUNC_ADDR(void(*)(ocrPolicyDomain_t*,void*), hcPdFree);
#ifdef OCR_ENABLE_STATISTICS
    base->policyDomainFcts.getStats = FUNC_ADDR(ocrStats_t*(*)(ocrPolicyDomain_t*),hcGetStats);
#endif

    return base;
}

#endif /* ENABLE_POLICY_DOMAIN_HC */