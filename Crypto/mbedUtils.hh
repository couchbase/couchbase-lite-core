//
// mbedUtils.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Error.hh"
#include "Logging.hh"
#include "function_ref.hh"

#ifdef __clang__
// mbed header doc-comments use "deprecated" in a way Clang doesn't like
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#endif

#include "ctr_drbg.h"
#include "x509_crt.h"
#include "error.h"

namespace litecore { namespace crypto {

    [[noreturn]] void throwMbedTLSError(int err);

    // Wrap this around mbed API calls that return an error code.
    static inline int TRY(int err) {
        if (_usuallyFalse(err < 0))
            throwMbedTLSError(err);
        return err;
    }

    // Converts X509 name structure to a string
    std::string getX509Name(mbedtls_x509_name *xname);

    // Returns a global random number context, initialized on 1st call.
    mbedtls_ctr_drbg_context* RandomNumberContext();

    // Utility wrapper for mbedTLS functions that write to a string buffer.
    alloc_slice allocString(size_t maxSize, function_ref<int(char*,size_t)> writer);

    // Utility wrapper for mbedTLS functions that write DER to a buffer
    alloc_slice allocDER(size_t maxSize, function_ref<int(uint8_t*,size_t)> writer);

    alloc_slice convertToPEM(const alloc_slice &derData, const char *name NONNULL);

} }
