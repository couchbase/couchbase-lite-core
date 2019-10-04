//
// mbedUtils.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "function_ref.hh"

struct mbedtls_asn1_named_data;
struct mbedtls_ctr_drbg_context;

namespace litecore { namespace crypto {

    [[noreturn]] void throwMbedTLSError(int err);

    // Wrap this around mbed API calls that return an error code.
    static inline int TRY(int err) {
        if (_usuallyFalse(err < 0))
            throwMbedTLSError(err);
        return err;
    }

    // Converts X509 name structure to a string
    fleece::alloc_slice getX509Name(mbedtls_asn1_named_data /*mbedtls_x509_name*/ *xname);

    // Returns a global random number context, initialized on 1st call.
    mbedtls_ctr_drbg_context* RandomNumberContext();

    // Utility wrapper for mbedTLS functions that write to a string buffer.
    fleece::alloc_slice allocString(size_t maxSize, function_ref<int(char*,size_t)> writer);

    // Utility wrapper for mbedTLS functions that write DER to a buffer
    fleece::alloc_slice allocDER(size_t maxSize, function_ref<int(uint8_t*,size_t)> writer);

    using ParseFn = int (*)(void*, const uint8_t*, size_t);

    void parsePEMorDER(slice data, const char *what, void *context, ParseFn);

    fleece::alloc_slice convertToPEM(const slice &derData, const char *name NONNULL);

} }
