
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/context.h
/// \brief Shared contexts allow multiple client modules to share the same
/// resources, including a single run thread, thread stack and message queue.
///
/// Shared contexts can reduce or altogether eliminate mutex contention since
/// clients in the same shared context are guaranteed to not preempt each other.
/// In general shared contexts are intended to run at lower thread priorities
/// than critical functionality. Shared contexts may be thought of as a form
/// of cooperative multitasking between clients where memory savings are a lot
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

/// Initialize context thread and data structures.
/// Queue depth should be ample enough to allow the contexts to initialize.
MOS_CLIENT_UNSAFE void mosInitContext(MosContext * pContext, MosThreadPriority prio, u8 * pStackBottom,
                                          u32 stackSize, MosContextMessage * pMsgQueueBuf,
                                          u32 msgQueueDepth);
/// Start context thread and client message processing.
///
MOS_CLIENT_UNSAFE void mosStartContext(MosContext * pContext);
/// Broadcast stop message to all clients to stop context and terminate context thread.
/// \note Must not be called by a client.
MOS_CLIENT_UNSAFE void mosStopContext(MosContext * pContext);
/// Wait until a context thread is finished.
/// \note Must not be called by a client.
MOS_CLIENT_UNSAFE void mosWaitForContextStop(MosContext * pContext);
/// Start an individual client and attach it to the context.
///  \note Message processing won't start until MosStartContext() is invoked. If clients are
///  started after MosStartContext() they will be sent start messages individually.
///  \note Must not be called by a client.
MOS_CLIENT_UNSAFE void mosStartClient(MosContext * pContext, MosClient * pClient,
                                         MosClientHandler * pHandler, void * pPrivData);
/// Send a stop client message.
/// \note A context will not terminate until a _broadcast_ stop message is sent
/// to the context. The Client will remain attached to the context until the _broadcast_
/// stop message is sent.
/// \note Must not be called by a client.
MOS_CLIENT_UNSAFE void mosStopClient(MosContext * pContext, MosClient * pClient);
/// Set a context message intended for a given client with a given message ID.
///
MOS_ISR_SAFE MOS_INLINE void
mosSetContextMessage(MosContextMessage * pMsg, MosClient * pClient, MosContextMessageID id) {
    pMsg->pClient = pClient;
    pMsg->id = id;
}
/// Set a context message intended for all clients with a given message ID.
///
MOS_ISR_SAFE MOS_INLINE void
mosSetContextBroadcastMessage(MosContextMessage * pMsg, MosContextMessageID id) {
    pMsg->pClient = NULL;
    pMsg->id = id;
}
/// Set pointer to message private data.
///
MOS_ISR_SAFE MOS_INLINE void
mosSetContextMessageData(MosContextMessage * pMsg, void * pData) {
    pMsg->pData = pData;
}
/// Send a message to a context if space in queue is available.
/// \note May safely be used in any context, recommended for inter-client messaging within
/// the same context.
MOS_ISR_SAFE MOS_INLINE bool
mosTrySendMessageToContext(MosContext * pContext, MosContextMessage * pMsg) {
    return mosTrySendToQueue(&pContext->msgQ, pMsg);
}
/// Send a inter-context message (external).
/// \note May safely be used only between different contexts, or from the outside world to a context.
MOS_INLINE void mosSendMessageToContext(MosContext * pContext, MosContextMessage * pMsg) {
    mosAssert(mosGetRunningThread() != &pContext->thd);
    mosSendToQueue(&pContext->msgQ, pMsg);
}

/* Context timer messages */

/// Initialize a mosTimer for use by a context.
///
void mosInitContextTimer(MosContextTimer * pTmr, MosContext * pContext);
/// Set a context timer.
///
MOS_INLINE void mosSetContextTimer(MosContextTimer * pTmr, u32 ticks, MosContextMessage * pMsg) {
    pTmr->msg = *pMsg;
    mosSetTimer(&pTmr->tmr, ticks, NULL);
}
/// Cancel a context timer.
///
MOS_INLINE void mosCancelContextTimer(MosContextTimer * pTmr) {
    mosCancelTimer(&pTmr->tmr);
}
/// Restart a context timer.
///
MOS_INLINE void mosResetContextTimer(MosContextTimer * pTmr) {
    mosResetTimer(&pTmr->tmr);
}

#endif
