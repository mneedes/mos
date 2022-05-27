
// Copyright 2019-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

#ifndef _MOS_FORMAT_STRING_H_
#define _MOS_FORMAT_STRING_H_

#include <stdarg.h>
#include <mos/defs.h>

/// Convert 32-bit integers to string, returns number of digits
///
u32 MosItoa(char * restrict out, s32 input, u16 base, bool is_upper,
            u16 min_width, char pad_char, bool is_signed);

/// Write up to size-1 bytes to destination buffer and terminate with nul character
///   Returns number of characters written had dest been sufficiently large,
///      not counting nul.
s32 MosVSNPrintf(char * dest, mos_size size, const char * fmt, va_list arg);
s32 MosSNPrintf(char * dest, mos_size size, const char * fmt, ...);

#endif
