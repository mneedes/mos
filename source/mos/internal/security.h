
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

// Internal interface for security context module

#ifndef _MOS_INTERNAL_SECURITY_H_
#define _MOS_INTERNAL_SECURITY_H_

#define MOS_DEFAULT_SECURE_CONTEXT        0

typedef void (MosSecKPrintHook)(void);

void _NSC_mosInitSecureContexts(MosSecKPrintHook * hook, char (*buffer)[MOS_PRINT_BUFFER_SIZE]);
void _NSC_mosResetSecureContext(s32 context);
void _NSC_mosSwitchSecureContext(s32 save_context, s32 restore_context);

#endif
