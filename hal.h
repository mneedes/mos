
//  Copyright 2019 Matthew Christopher Needes
//  Licensed under the terms and conditions contained within the LICENSE
//  file (the "License") included under this distribution.  You may not use
//  this file except in compliance with the License.

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
