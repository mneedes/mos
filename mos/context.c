
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// (Shared) Context
//

// TODO: Should support for this be added to dynamic threads?
// TODO: Should they be ClientMessages or ContextMessages?  ClientTimers or ContextTimers?

#include <mos/context.h>

static s32 ContextRunner(s32 in) {
    MosContext * context = (MosContext *) in;
    bool running = true;
    while (running) {
        MosContextMessage msg;
        MosReceiveFromQueue(&context->msg_q, &msg);
        MosClient * client = msg.client;
        if (client) {
            // Only send queued resume message if client still needs it.
            if (msg.id != MosContextMessageID_ResumeClient || !client->completed) {
                // Unicast message (NOTE: client is allowed to modify msg)
                client->completed = (*client->handler)(&msg);
                if (client->completed) {
                    if (MosIsOnList(&client->resume_link))
                        MosRemoveFromList(&client->resume_link);
                } else if (!MosIsOnList(&client->resume_link)) {
                    MosAddToList(&context->resume_q, &client->resume_link);
                }
            }
        } else {
            // Broadcast message
            if (msg.id == MosContextMessageID_StopContext) {
                // Stop all clients if stopping context and terminate thread
                msg.id = MosContextMessageID_StopClient;
                running = false;
            }
            MosLink * elm;
            MosLockMutex(&context->mtx);
            for (elm = context->client_q.next; elm != &context->client_q; elm = elm->next) {
                MosClient * client = container_of(elm, MosClient, client_link);
                // Copy the message since client is allowed to alter messages
                MosContextMessage msg_copy = { .id = msg.id, .client = client, .data = msg.data };
                client->completed = (*client->handler)(&msg_copy);
                if (client->completed) {
                    if (MosIsOnList(&client->resume_link))
                        MosRemoveFromList(&client->resume_link);
                } else if (!MosIsOnList(&client->resume_link)) {
                    MosAddToList(&context->resume_q, &client->resume_link);
                }
            }
            MosUnlockMutex(&context->mtx);
        }
        // Attempt to resume clients
        MosLink * elm_save;
        for (MosLink * elm = context->resume_q.next; elm != &context->resume_q; elm = elm_save) {
            elm_save = elm->next;
            msg.client = container_of(elm, MosClient, resume_link);
            // Don't bother resuming if client already completed after processing a subsequent message
            if (!msg.client->completed) {
                msg.id = MosContextMessageID_ResumeClient;
                if (!MosTrySendToQueue(&context->msg_q, &msg)) break;
            }
            MosRemoveFromList(&msg.client->resume_link);
        }
    }
    return 0;
}

void MosInitContext(MosContext * context, MosThreadPriority prio, u8 * stack_bottom,
                       u32 stack_size, MosContextMessage * msg_queue_buf, u32 msg_queue_depth) {
    MosInitMutex(&context->mtx);
    MosInitList(&context->client_q);
    MosInitList(&context->resume_q);
    MosInitQueue(&context->msg_q, msg_queue_buf, sizeof(MosContextMessage), msg_queue_depth);
    MosInitThread(&context->thd, prio, ContextRunner, (s32) context, stack_bottom, stack_size);
}

void MosStartContext(MosContext * context) {
    MosLockMutex(&context->mtx);
    MosRunThread(&context->thd);
    MosContextMessage msg = { .id = MosContextMessageID_StartClient, .client = NULL };
    MosSendMessageToContext(context, &msg);
    MosUnlockMutex(&context->mtx);
}

void MosStopContext(MosContext * context) {
    MosContextMessage msg = { .id = MosContextMessageID_StopContext, .client = NULL };
    MosSendMessageToContext(context, &msg);
}

void MosWaitForContextStop(MosContext * context) {
    MosWaitForThreadStop(&context->thd);
}

void MosStartClient(MosContext * context, MosClient * client, MosClientHandler * handler, void * priv_data) {
    client->handler = handler;
    client->priv_data = priv_data;
    client->completed = true;
    MosInitList(&client->resume_link);
    MosLockMutex(&context->mtx);
    MosAddToList(&context->client_q, &client->client_link);
    if (MosGetThreadState(&context->thd, NULL) != MOS_THREAD_NOT_STARTED) {
        MosContextMessage msg = { .id = MosContextMessageID_StartClient, .client = client };
        MosSendMessageToContext(context, &msg);
    }
    MosUnlockMutex(&context->mtx);
}

void MosStopClient(MosContext * context, MosClient * client) {
    MosContextMessage msg = { .id = MosContextMessageID_StopClient, .client = client };
    MosSendMessageToContext(context, &msg);
}

MOS_ISR_SAFE static bool ContextTimerCallback(MosTimer * _tmr) {
    MosContextTimer * tmr = container_of(_tmr, MosContextTimer, tmr);
    return MosTrySendMessageToContext(tmr->context, &tmr->msg);
}

void MosInitContextTimer(MosContextTimer * tmr, MosContext * context) {
    tmr->context = context;
    MosInitTimer(&tmr->tmr, ContextTimerCallback);
}
