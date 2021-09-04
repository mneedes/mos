
// Copyright 2019-2021 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// MOS Test Bench
//

#ifndef _MOS_TEST_BENCH_H_
#define _MOS_TEST_BENCH_H_

typedef enum {
   TRACE_DEBUG       = 1 << 0,
   TRACE_INFO        = 1 << 1,
   TRACE_ERROR       = 1 << 2,
   TRACE_FATAL       = 1 << 3,
} TraceLevel;

int InitTestBench(void);

#endif
