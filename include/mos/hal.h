
// Copyright 2019-2022 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

//
// Application HAL definitions
//   Use this API to implement your BSP
//

#ifndef _MOS_HAL_H_
#define _MOS_HAL_H_

#include <mos/defs.h>

typedef void (HalRxUARTCallback)(char ch);

void HalInit(void);
void HalRegisterRxUARTCallback(HalRxUARTCallback * rx_callback);
void HalSendToTxUART(char ch);

// TODO: There might be a better place for these
u32 HalGetRandomU32(void);
void HalSetGpio(u32 num, bool value);

#endif
