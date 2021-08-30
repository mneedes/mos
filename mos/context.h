
//  Copyright 2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// (Shared) Context
//   Shared contexts allow multiple client modules to share the same resources,
//   including a single run thread, thread stack and message queue. In addition,
//   shared contexts can reduce or eliminate mutex contention since clients are
//   guaranteed to not preempt each other. Contexts are essentially a form of
//   cooperative multitasking where memory savings are more important than
//   deadlines.
//
//   Contexts use a shared message queue for inter-client communication. The
//   maximum latency depends on the maximum processing time for all messages
//   in the context. Context clients should be implemented as state machines
//   that multiplex received messages. Contexts can send and receive messages
//   to or from ISRs or other threads on the system. Clients should not block
//   or wait very long otherwise they might starve other clients sharing the
//   same context. The MosTrySendMessageToContext() call should be used when
//   a client sends a message to another client in the same context.
//

#ifndef _MOS_CONTEXT_H_
#define _MOS_CONTEXT_H_

#include <mos/kernel.h>
#include <mos/queue.h>
#include <mos/trace.h>

// Marks calls that unsafe in client handlers
#define MOS_CLIENT_UNSAFE

// TODO: should this all be part of the dynamic threads?

typedef enum {
    MosContextMessageID_Start            = 0xFFFFFFFD,  /* Called during initialization */
    MosContextMessageID_Stop             = 0xFFFFFFFE,  /* Called during shutdown */
    MosContextMessageID_Resume           = 0xFFFFFFFF,  /* Handler is resumed */
    MosContextMessageID_FirstUserMessage = 0x00000000,  /* First user-defined message */
} MosContextMessageID;

struct MosClient;

typedef struct {
    struct MosClient * client;  /* If NULL, message is broadcast to entire context */
    u32                id;      /* Message ID */
    u32              * payload; /* Message payload */
} MosContextMessage;

// Context handler returns false if its task hasn't completed.
//   The message queue will be allowed to drain and the handler will get a resume message.
typedef bool (MosClientHandler)(MosContextMessage *);

typedef struct MosClient {
    MosLink            client_link;
    MosLink            resume_link;
    MosClientHandler * handler;
    void             * priv_data;
    bool               completed;
} MosClient;

typedef struct {
    MosMutex   mtx;
    MosQueue   msg_q;
    MosList    client_q;
    MosList    resume_q;
    MosThread  thd;
} MosContext;

// The following calls must not be invoked inside of Client Handlers (MOS_CLIENT_UNSAFE)
MOS_CLIENT_UNSAFE void MosStartContext(MosContext * context, MosThreadPriority prio, u32 stack_size, u32 msg_queue_depth);
MOS_CLIENT_UNSAFE void MosStopContext(MosContext * context);
MOS_CLIENT_UNSAFE void MosWaitForContextStop(MosContext * context);
MOS_CLIENT_UNSAFE void MosStartClient(MosContext * context, MosClient * client, MosClientHandler * handler, void * priv_data);
MOS_CLIENT_UNSAFE void MosStopClient(MosContext * context, MosClient * client);

MOS_ISR_SAFE MOS_INLINE void
MosSetContextMessage(MosContextMessage * msg, MosClient * client, MosContextMessageID id) {
    msg->client = client;
    msg->id = id;
}

MOS_ISR_SAFE MOS_INLINE void
MosSetContextBroadcastMessage(MosContextMessage * msg, MosContextMessageID id) {
    msg->client = NULL;
    msg->id = id;
}

MOS_ISR_SAFE MOS_INLINE void
MosSetContextMessagePayload(MosContextMessage * msg, void * payload) {
    msg->payload = payload;
}

/* May safely be used in any context, recommended for inter-client messaging within the same context */
MOS_ISR_SAFE MOS_INLINE bool MosTrySendMessageToContext(MosContext * context, MosContextMessage * msg) {
    return MosTrySendToQueue(&context->msg_q, msg);
}

/* May safely be used only inter-context (between contexts). */
MOS_INLINE void MosSendMessageToContext(MosContext * context, MosContextMessage * msg) {
    MosAssert(MosGetThreadPtr() != &context->thd);
    MosSendToQueue(&context->msg_q, msg);
}

#endif
