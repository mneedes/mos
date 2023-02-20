
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// (Shared) Context
//

#include <mos/context.h>

static s32 ContextRunner(s32 in) {
    MosContext * pContext = (MosContext *)in;
    bool running = true;
    while (running) {
        MosContextMessage msg;
        mosReceiveFromQueue(&pContext->msgQ, &msg);
        MosClient * pClient = msg.pClient;
        if (pClient) {
            // Only send queued resume message if client still needs it.
            if (msg.id != MosContextMessageID_ResumeClient || !pClient->completed) {
                // Unicast message (NOTE: client is allowed to modify msg)
                pClient->completed = (*pClient->pHandler)(&msg);
                if (pClient->completed) {
                    if (mosIsOnList(&pClient->resumeLink))
                        mosRemoveFromList(&pClient->resumeLink);
                } else if (!mosIsOnList(&pClient->resumeLink)) {
                    mosAddToEndOfList(&pContext->resumeQ, &pClient->resumeLink);
                }
            }
        } else {
            // Broadcast message
            if (msg.id == MosContextMessageID_StopContext) {
                // Stop all clients if stopping context and terminate thread
                msg.id = MosContextMessageID_StopClient;
                running = false;
            }
            MosLink * pElm;
            mosLockMutex(&pContext->mtx);
            for (pElm = pContext->clientQ.pNext; pElm != &pContext->clientQ; pElm = pElm->pNext) {
                MosClient * pClient = container_of(pElm, MosClient, clientLink);
                // Copy the message since client is allowed to alter messages
                MosContextMessage msg_copy = { .id = msg.id, .pClient = pClient, .pData = msg.pData };
                pClient->completed = (*pClient->pHandler)(&msg_copy);
                if (pClient->completed) {
                    if (mosIsOnList(&pClient->resumeLink))
                        mosRemoveFromList(&pClient->resumeLink);
                } else if (!mosIsOnList(&pClient->resumeLink)) {
                    mosAddToEndOfList(&pContext->resumeQ, &pClient->resumeLink);
                }
            }
            mosUnlockMutex(&pContext->mtx);
        }
        // Attempt to resume clients
        MosLink * pElmSave;
        for (MosLink * pElm = pContext->resumeQ.pNext; pElm != &pContext->resumeQ; pElm = pElmSave) {
            pElmSave = pElm->pNext;
            msg.pClient = container_of(pElm, MosClient, resumeLink);
            // Don't bother resuming if client already completed after processing a subsequent message
            if (!msg.pClient->completed) {
                msg.id = MosContextMessageID_ResumeClient;
                if (!mosTrySendToQueue(&pContext->msgQ, &msg)) break;
            }
            mosRemoveFromList(&msg.pClient->resumeLink);
        }
    }
    return 0;
}

void mosInitContext(MosContext * pContext, MosThreadPriority prio, u8 * pStackbottom,
                       u32 stackSize, MosContextMessage * pMsgQueueBuf, u32 msgQueueDepth) {
    mosInitMutex(&pContext->mtx);
    mosInitList(&pContext->clientQ);
    mosInitList(&pContext->resumeQ);
    mosInitQueue(&pContext->msgQ, pMsgQueueBuf, sizeof(MosContextMessage), msgQueueDepth);
    mosInitThread(&pContext->thd, prio, ContextRunner, (s32)pContext, pStackbottom, stackSize);
}

void mosStartContext(MosContext * pContext) {
    mosLockMutex(&pContext->mtx);
    mosRunThread(&pContext->thd);
    MosContextMessage msg = { .id = MosContextMessageID_StartClient, .pClient = NULL };
    mosSendMessageToContext(pContext, &msg);
    mosUnlockMutex(&pContext->mtx);
}

void mosStopContext(MosContext * pContext) {
    MosContextMessage msg = { .id = MosContextMessageID_StopContext, .pClient = NULL };
    mosSendMessageToContext(pContext, &msg);
}

void mosWaitForContextStop(MosContext * pContext) {
    mosWaitForThreadStop(&pContext->thd);
}

void mosStartClient(MosContext * pContext, MosClient * pClient, MosClientHandler * pHandler, void * pPrivData) {
    pClient->pHandler = pHandler;
    pClient->pPrivData = pPrivData;
    pClient->completed = true;
    mosInitList(&pClient->resumeLink);
    mosLockMutex(&pContext->mtx);
    mosAddToEndOfList(&pContext->clientQ, &pClient->clientLink);
    if (mosGetThreadState(&pContext->thd, NULL) != MOS_THREAD_NOT_STARTED) {
        MosContextMessage msg = { .id = MosContextMessageID_StartClient, .pClient = pClient };
        mosSendMessageToContext(pContext, &msg);
    }
    mosUnlockMutex(&pContext->mtx);
}

void mosStopClient(MosContext * pContext, MosClient * pClient) {
    MosContextMessage msg = { .id = MosContextMessageID_StopClient, .pClient = pClient };
    mosSendMessageToContext(pContext, &msg);
}

MOS_ISR_SAFE static bool ContextTimerCallback(MosTimer * _pTmr) {
    MosContextTimer * pTmr = container_of(_pTmr, MosContextTimer, tmr);
    return mosTrySendMessageToContext(pTmr->pContext, &pTmr->msg);
}

void mosInitContextTimer(MosContextTimer * pTmr, MosContext * pContext) {
    pTmr->pContext = pContext;
    mosInitTimer(&pTmr->tmr, ContextTimerCallback);
}
