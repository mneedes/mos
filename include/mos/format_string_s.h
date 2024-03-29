
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_FORMAT_STRING_S_H_
#define _MOS_FORMAT_STRING_S_H_

#include <stdarg.h>
#include <mos/defs.h>

s32 S_mosVSNPrintf(char * pDest, mos_size size, const char * pFmt, va_list arg);
s32 S_mosSNPrintf(char * pDest, mos_size size, const char * pFmt, ...);

#endif
