
//  Copyright 2019-2020 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

#ifndef _HAL_TB_H_
#define _HAL_TB_H_

#include <bsp_hal.h>

void MOS_ISR_SAFE IRQ0_Callback(void);
void MOS_ISR_SAFE IRQ1_Callback(void);

void HalTestsInit(void);
void HalTestsTriggerInterrupt(u32 num);
bool HalTests(int argc, char * argv[]);

#endif
