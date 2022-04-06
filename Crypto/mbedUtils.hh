//
// mbedUtils.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "fleece/function_ref.hh"

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

    using ParseCallback = function_ref<int(const uint8_t*, size_t)>;

    // Invokes the callback with the given data. If the data is in PEM format, it will be
    // null-terminated when passed to the callback (mbedTLS expects this.)
    // If the callback returns nonzero, it's thrown as an mbedTLS error.
    void parsePEMorDER(slice data, const char *what, ParseCallback);

    // alternative form that takes a C function directly, like `mbedtls_pk_parse_public_key`.
    template <typename CTX>
    void parsePEMorDER(slice data, const char *what, CTX *context,
                       int (*cfn)(CTX*, const uint8_t*, size_t))
    {
        parsePEMorDER(data, what, [=](const uint8_t* bytes, size_t size) {
            return cfn(context, bytes, size);
        });
    }


    fleece::alloc_slice convertToPEM(const slice &derData, const char *name NONNULL);

} }
