
//  Copyright 2019 Matthew C Needes
//  You may not use this source file except in compliance with the
//  terms and conditions contained within the LICENSE file (the
//  "License") included under this distribution.

//
// Application HAL definitions
//   Use this API to implement your BSP
//

#ifndef _HAL_H_
#define _HAL_H_

#include "mos_phal.h"

void HalInit(void);
void HalPrintToConsole(char *str);

#endif
