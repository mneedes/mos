
// Copyright 2021-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/context.h
/// \brief Shared Contexts
///
/// Shared contexts allow multiple client modules to share the same resources,
/// including a single run thread, thread stack and message queue. Shared
/// contexts can reduce or altogether eliminate mutex contention since clients
/// in the same shared context are guaranteed to not preempt each other. In
/// general shared contexts probably should be implemented at lower thread
/// priorities than most other functionality. Shared contexts may be thought
/// of as a form of cooperative multitasking where memory savings are a lot
/// more important than deadlines.
///
/// Contexts use a shared message queue for inter-client communication. The
/// maximum latency depends on the maximum processing time for all messages
/// in the context. Context clients can be implemented as state machines
/// that multiplex received messages. Contexts can send and receive messages
/// to or from ISRs or other threads on the system. Clients should not block
/// or wait very long otherwise they might starve other clients sharing the
/// same context. The MosTrySendMessageToContext() call should be used when
/// a client sends a message to another client in the same context.
///
/// If a client handler has not completed and desires a callback, it should
/// return false. Note that the callback is implemented as a resume message on
/// the same message queue. The resume message will be added to the end of the
/// queue allowing the opportunity for other messages to drain first.
///
/// Client handlers state machines should tolerate receiving messages after
/// being _individually_ stopped. This includes, but is not limited to
/// broadcast stop messages, resume messages or potentially even user messages
/// (depending on the system design and how the context is used).
///
/// The overall context will only shutdown upon receipt of a _broadcast_
/// StopContext message and any messages that are queued beyond a _broadcast_
/// StopContextmessage will be ignored. Any clients requesting resume after
/// receiving a broadcast StopContext stop message will also be ignored. All
/// clients will receive a StopClient message upon receipt of a _broadcast_
/// StopContext message (even if already stopped).

#ifndef _MOS_CONTEXT_H_
#define _MOS_CONTEXT_H_

#include <mos/kernel.h>
#include <mos/queue.h>
#include <mos/trace.h>

// Marks calls that are unsafe in client handlers
#define MOS_CLIENT_UNSAFE

typedef u32 MosContextMessageID;
enum MosContextMessageID {
    MosContextMessageID_StartClient      = 0xFFFFFFFC,  /* Request client initialization */
    MosContextMessageID_StopClient       = 0xFFFFFFFD,  /* Request client shutdown */
    MosContextMessageID_ResumeClient     = 0xFFFFFFFE,  /* Request resumption of client handler */
    MosContextMessageID_StopContext      = 0xFFFFFFFF,  /* Shutdown entire context (broadcast only) */
    MosContextMessageID_FirstUserMessage = 0x00000000,  /* First user-defined message */
};

struct MosClient;

typedef struct {
    struct MosClient    * pClient; /* Destination Client. If NULL, message is broadcast */
    MosContextMessageID   id;      /* Message ID */
    void                * pData;   /* User data (e.g.: Message Payload) */
} MosContextMessage;


// Client handlers are callbacks that process incoming messages.
//   Client handlers return false if their task hasn't completed and they desire another
//   callback. The message queue will be allowed to drain and the handler will get a resume
//   message.
typedef bool (MosClientHandler)(MosContextMessage *);

typedef struct MosClient {
    MosClientHandler * pHandler;
    void             * pPrivData;
    MosLink            clientLink;
    MosLink            resumeLink;
    bool               completed;
} MosClient;

typedef struct {
    MosMutex   mtx;
    MosQueue   msgQ;
    MosList    clientQ;
    MosList    resumeQ;
    MosThread  thd;
} MosContext;

typedef struct {
    MosTimer           tmr;      /* Timer */
    MosContext       * pContext; /* Context */
    MosContextMessage  msg;      /* Message to send on Timer Expiration */
} MosContextTimer;

// The following calls must not be invoked inside of Client Handlers (MOS_CLIENT_UNSAFE)

// Initialize context thread and data structure
// Queue depth should be ample enough to allow the contexts to initialize
MOS_CLIENT_UNSAFE void MosInitContext(MosContext * pContext, MosThreadPriority prio, u8 * pStackBottom,
                                          u32 stackSize, MosContextMessage * pMsgQueueBuf,
                                          u32 msgQueueDepth);
// Start context thread
MOS_CLIENT_UNSAFE void MosStartContext(MosContext * pContext);
// Broadcast stop message to all clients to stop context and terminate context thread
MOS_CLIENT_UNSAFE void MosStopContext(MosContext * pContext);
// Wait until a context is finished
MOS_CLIENT_UNSAFE void MosWaitForContextStop(MosContext * pContext);
// Start an individual client and attach it to the context,
//   Note that clients won't actually start until MosStartContext() is invoked for the first
//   time. If clients are started after MosStartContext() they will be sent start messages
//   individually.
MOS_CLIENT_UNSAFE void MosStartClient(MosContext * pContext, MosClient * pClient,
                                         MosClientHandler * pHandler, void * pPrivData);
// Send a stop client message,
//   note that the context will not terminate until a _broadcast_ stop message is sent
//   to the context. The Client will remain attached to the context until the _broadcast_
//   stop message is sent.
MOS_CLIENT_UNSAFE void MosStopClient(MosContext * pContext, MosClient * pClient);

MOS_ISR_SAFE MOS_INLINE void
MosSetContextMessage(MosContextMessage * pMsg, MosClient * pClient, MosContextMessageID id) {
    pMsg->pClient = pClient;
    pMsg->id = id;
}
MOS_ISR_SAFE MOS_INLINE void
MosSetContextBroadcastMessage(MosContextMessage * pMsg, MosContextMessageID id) {
    pMsg->pClient = NULL;
    pMsg->id = id;
}
MOS_ISR_SAFE MOS_INLINE void
MosSetContextMessageData(MosContextMessage * pMsg, void * pData) {
    pMsg->pData = pData;
}
/* May safely be used in any context, recommended for inter-client messaging within the same context */
MOS_ISR_SAFE MOS_INLINE bool
MosTrySendMessageToContext(MosContext * pContext, MosContextMessage * pMsg) {
    return MosTrySendToQueue(&pContext->msgQ, pMsg);
}
/* May safely be used only inter-context (between contexts). */
MOS_INLINE void MosSendMessageToContext(MosContext * pContext, MosContextMessage * pMsg) {
    MosAssert(MosGetThreadPtr() != &pContext->thd);
    MosSendToQueue(&pContext->msgQ, pMsg);
}

/* Context timer messages */

void MosInitContextTimer(MosContextTimer * pTmr, MosContext * pContext);
MOS_INLINE void MosSetContextTimer(MosContextTimer * pTmr, u32 ticks, MosContextMessage * pMsg) {
    pTmr->msg = *pMsg;
    MosSetTimer(&pTmr->tmr, ticks, NULL);
}
MOS_INLINE void MosCancelContextTimer(MosContextTimer * pTmr) {
    MosCancelTimer(&pTmr->tmr);
}
MOS_INLINE void MosResetContextTimer(MosContextTimer * pTmr) {
    MosResetTimer(&pTmr->tmr);
}

#endif
