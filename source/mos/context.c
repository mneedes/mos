
// Copyright 2021 Matthew C Needes
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
        MosReceiveFromQueue(&pContext->msgQ, &msg);
        MosClient * pClient = msg.pClient;
        if (pClient) {
            // Only send queued resume message if client still needs it.
            if (msg.id != MosContextMessageID_ResumeClient || !pClient->completed) {
                // Unicast message (NOTE: client is allowed to modify msg)
                pClient->completed = (*pClient->pHandler)(&msg);
                if (pClient->completed) {
                    if (MosIsOnList(&pClient->resumeLink))
                        MosRemoveFromList(&pClient->resumeLink);
                } else if (!MosIsOnList(&pClient->resumeLink)) {
                    MosAddToList(&pContext->resumeQ, &pClient->resumeLink);
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
            MosLockMutex(&pContext->mtx);
            for (pElm = pContext->clientQ.pNext; pElm != &pContext->clientQ; pElm = pElm->pNext) {
                MosClient * pClient = container_of(pElm, MosClient, clientLink);
                // Copy the message since client is allowed to alter messages
                MosContextMessage msg_copy = { .id = msg.id, .pClient = pClient, .pData = msg.pData };
                pClient->completed = (*pClient->pHandler)(&msg_copy);
                if (pClient->completed) {
                    if (MosIsOnList(&pClient->resumeLink))
                        MosRemoveFromList(&pClient->resumeLink);
                } else if (!MosIsOnList(&pClient->resumeLink)) {
                    MosAddToList(&pContext->resumeQ, &pClient->resumeLink);
                }
            }
            MosUnlockMutex(&pContext->mtx);
        }
        // Attempt to resume clients
        MosLink * pElmSave;
        for (MosLink * pElm = pContext->resumeQ.pNext; pElm != &pContext->resumeQ; pElm = pElmSave) {
            pElmSave = pElm->pNext;
            msg.pClient = container_of(pElm, MosClient, resumeLink);
            // Don't bother resuming if client already completed after processing a subsequent message
            if (!msg.pClient->completed) {
                msg.id = MosContextMessageID_ResumeClient;
                if (!MosTrySendToQueue(&pContext->msgQ, &msg)) break;
            }
            MosRemoveFromList(&msg.pClient->resumeLink);
        }
    }
    return 0;
}

void MosInitContext(MosContext * pContext, MosThreadPriority prio, u8 * pStackbottom,
                       u32 stackSize, MosContextMessage * pMsgQueueBuf, u32 msgQueueDepth) {
    MosInitMutex(&pContext->mtx);
    MosInitList(&pContext->clientQ);
    MosInitList(&pContext->resumeQ);
    MosInitQueue(&pContext->msgQ, pMsgQueueBuf, sizeof(MosContextMessage), msgQueueDepth);
    MosInitThread(&pContext->thd, prio, ContextRunner, (s32)pContext, pStackbottom, stackSize);
}

void MosStartContext(MosContext * pContext) {
    MosLockMutex(&pContext->mtx);
    MosRunThread(&pContext->thd);
    MosContextMessage msg = { .id = MosContextMessageID_StartClient, .pClient = NULL };
    MosSendMessageToContext(pContext, &msg);
    MosUnlockMutex(&pContext->mtx);
}

void MosStopContext(MosContext * pContext) {
    MosContextMessage msg = { .id = MosContextMessageID_StopContext, .pClient = NULL };
    MosSendMessageToContext(pContext, &msg);
}

void MosWaitForContextStop(MosContext * context) {
    MosWaitForThreadStop(&context->thd);
}

void MosStartClient(MosContext * pContext, MosClient * pClient, MosClientHandler * pHandler, void * pPrivData) {
    pClient->pHandler = pHandler;
    pClient->pPrivData = pPrivData;
    pClient->completed = true;
    MosInitList(&pClient->resumeLink);
    MosLockMutex(&pContext->mtx);
    MosAddToList(&pContext->clientQ, &pClient->clientLink);
    if (MosGetThreadState(&pContext->thd, NULL) != MOS_THREAD_NOT_STARTED) {
        MosContextMessage msg = { .id = MosContextMessageID_StartClient, .pClient = pClient };
        MosSendMessageToContext(pContext, &msg);
    }
    MosUnlockMutex(&pContext->mtx);
}

void MosStopClient(MosContext * pContext, MosClient * pClient) {
    MosContextMessage msg = { .id = MosContextMessageID_StopClient, .pClient = pClient };
    MosSendMessageToContext(pContext, &msg);
}

MOS_ISR_SAFE static bool ContextTimerCallback(MosTimer * _pTmr) {
    MosContextTimer * pTmr = container_of(_pTmr, MosContextTimer, tmr);
    return MosTrySendMessageToContext(pTmr->pContext, &pTmr->msg);
}

void MosInitContextTimer(MosContextTimer * pTmr, MosContext * pContext) {
    pTmr->pContext = pContext;
    MosInitTimer(&pTmr->tmr, ContextTimerCallback);
}
