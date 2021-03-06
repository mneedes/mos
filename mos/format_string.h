
//  Copyright 2019-2021 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#ifndef _MOS_FORMAT_STRING_H_
#define _MOS_FORMAT_STRING_H_

#include <stdarg.h>
#include <mos/defs.h>

// Convert integers, long long integers and double-precision floating point to ASCII
u32 MosItoa(char * restrict out, s32 input, u16 base, bool is_upper,
            u16 min_digits, char pad_char, bool is_signed);
u32 MosLLtoa(char * restrict out, s64 input, u16 base, bool is_upper,
             u16 min_digits, char pad_char, bool is_signed);
u32 MosDtoa(char * restrict out, double in, u16 min_width, u16 prec);

// Like C vsnprintf()/vsnprintf() EXCEPT that it will always return number of
//   actual characters printed rather than what would have been printed.
s32 MosVSNPrintf(char * dest, mos_size size, const char * fmt, va_list arg);
s32 MosSNPrintf(char * dest, mos_size size, const char * fmt, ...);

#endif
