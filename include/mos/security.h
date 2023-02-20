
// Copyright 2021-2023 Matthew C Needes
// You may not use this source file except in compliance with the
// terms and conditions contained within the LICENSE file (the
// "License") included under this distribution.

/// \file  mos/security.h
/// \brief Security context management.
///
/// Allows reservation of security contexts from non-secure side

#ifndef _MOS_SECURITY_H_
#define _MOS_SECURITY_H_

#include <mos/static_kernel.h>

/// Reserve a security context for thread (blocking)
/// Invoke this before calling into security APIs layers such as TrustZone.
/// Blocks until a context is available.
void mosReserveSecureContext(void);

/// Reserve a security context for thread (non-blocking)
/// Invoke this before calling into security APIs layers such as TrustZone.
/// \return true if context available.
bool mosTryReserveSecureContext(void);

/// Release a security context for thread
/// Must invoke this when releasing context.
void mosReleaseSecureContext(void);

#endif
