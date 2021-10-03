
// Copyright 2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

// Internal interface for security context module

#ifndef _MOS_INTERNAL_SECURITY_H_
#define _MOS_INTERNAL_SECURITY_H_

#define MOS_DEFAULT_SECURE_CONTEXT        0

void _MosInitSecureContexts(void);
void _MosResetSecureContext(s32 context);
void _MosSwitchSecureContext(s32 save_context, s32 restore_context);

#endif
