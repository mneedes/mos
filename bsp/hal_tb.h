
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#ifndef _HAL_TB_H_
#define _HAL_TB_H_

#include "stm32f4xx_hal.h"

bool HalTests(MosThread * threads[], u32 max_threads, u8 * stacks[], u32 stack_size);
void HalSetGpio(u32 num, bool value);

#endif
