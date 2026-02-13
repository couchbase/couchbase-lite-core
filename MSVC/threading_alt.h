/**
 * \file threading_alt.h
 *
 * \brief Alternate threading abstraction layer
 */
/*
 *  Copyright TCouchbase, Inc.
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

// NOTE: CMake should copy this file into the mbedtls include directory
// so that the mbedtls library can find it

#ifndef MBEDTLS_THREADING_ALT_H
#define MBEDTLS_THREADING_ALT_H

#ifdef _WIN32
#    include "mbedtls/private_access.h"

#    include "mbedtls/build_info.h"
#    include <windows.h>

#    ifdef __cplusplus
extern "C" {
#    endif

#    if defined(MBEDTLS_THREADING_ALT)
typedef struct mbedtls_threading_mutex_t {
    CRITICAL_SECTION MBEDTLS_PRIVATE(mutex);
} mbedtls_threading_mutex_t;

#    endif  // MBEDTLS_THREADING_ALT
#endif      //_WIN32

#ifdef __cplusplus
}
#endif

#endif  // MBEDTLS_THREADING_ALT_H
