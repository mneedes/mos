
//  Copyright 2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// (Shared) Context
//

// TODO: should this all be part of the dynamic threads?
// TODO: starting/stopping/restarting clients is problematic
// TODO: what happens on resume on a client stop?

#include <mos/context.h>

static s32 ContextRunner(s32 in) {
    MosContext * context = (MosContext *) in;
    while (1) {
        MosContextMessage msg;
        MosReceiveFromQueue(&context->msg_q, &msg);
        MosClient * client = msg.client;
        if (client) {
            // Only send queued resume message if client still needs it.
            if (msg.id != MosContextMessageID_Resume || !client->completed) {
                // Unicast message
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
            MosLink * elm;
            MosLockMutex(&context->mtx);
            for (elm = context->client_q.next; elm != &context->client_q; elm = elm->next) {
                MosClient * client = container_of(elm, MosClient, client_link);
                // Copy the message since client is allowed to alter messages
                MosContextMessage msg_copy = { .id = msg.id, .client = client, .payload = msg.payload };
                client->completed = (*client->handler)(&msg_copy);
                if (client->completed) {
                    if (MosIsOnList(&client->resume_link))
                        MosRemoveFromList(&client->resume_link);
                } else if (!MosIsOnList(&client->resume_link)) {
                    MosAddToList(&context->resume_q, &client->resume_link);
                }
            }
            MosUnlockMutex(&context->mtx);
            // Shutdown terminates thread
            if (msg.id == MosContextMessageID_Stop) break;
        }
        // Attempt to resume clients
        MosLink * elm_save;
        for (MosLink * elm = context->resume_q.next; elm != &context->resume_q; elm = elm_save) {
            elm_save = elm->next;
            msg.client = container_of(elm, MosClient, resume_link);
            // Don't bother resuming if client already completed
            if (!msg.client->completed) {
                msg.id = MosContextMessageID_Resume;
                if (!MosTrySendToQueue(&context->msg_q, &msg)) break;
            }
            MosRemoveFromList(&msg.client->resume_link);
        }
    }
    return 0;
}

static u8 AppStack[1024];
static MosContextMessage myQueue[16];

void MosInitContext(MosContext * context, MosThreadPriority prio, u32 stack_size, u32 msg_queue_depth) {
    MosInitMutex(&context->mtx);
    MosInitList(&context->client_q);
    MosInitList(&context->resume_q);
    MosInitQueue(&context->msg_q, myQueue, sizeof(MosContextMessage), msg_queue_depth);
    MosInitThread(&context->thd, prio, ContextRunner, (s32) context, AppStack, stack_size);
}

void MosStartContext(MosContext * context) {
    MosRunThread(&context->thd);
    MosContextMessage msg = { .id = MosContextMessageID_Start, .client = NULL };
    MosSendMessageToContext(context, &msg);
}

void MosStopContext(MosContext * context) {
    MosContextMessage msg = { .id = MosContextMessageID_Stop, .client = NULL };
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
    MosUnlockMutex(&context->mtx);
    if (MosGetThreadState(&context->thd, NULL) != MOS_THREAD_NOT_STARTED) {
        MosContextMessage msg = { .id = MosContextMessageID_Start, .client = client };
        MosSendMessageToContext(context, &msg);
    }
}

void MosStopClient(MosContext * context, MosClient * client) {
    MosContextMessage msg = { .id = MosContextMessageID_Stop, .client = client };
    MosSendMessageToContext(context, &msg);
}
