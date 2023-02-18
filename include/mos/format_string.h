
// Copyright 2019-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_FORMAT_STRING_H_
#define _MOS_FORMAT_STRING_H_

#include <stdarg.h>
#include <mos/defs.h>

/// Convert 32-bit integers to string, returns number of digits
///
u32 mosItoa(char * restrict pOut, s32 input, u16 base, bool isUpper,
            u16 minWidth, char padChar, bool isSigned);

/// Write up to size-1 bytes to destination buffer and terminates with null character
///   Returns number of characters written had pDest been sufficiently large,
///      not counting the null.
s32 mosVSNPrintf(char * pDest, mos_size size, const char * pFmt, va_list arg);
s32 mosSNPrintf(char * pDest, mos_size size, const char * pFmt, ...);

#endif
