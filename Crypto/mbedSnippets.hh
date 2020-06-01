//
// mbedSnippets.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/x509_crt.h"
#pragma clang diagnostic pop


namespace litecore::crypto {

    // Compare two X.509 names (returns 0 if equal)
    int x509_name_cmp( const mbedtls_x509_name *a, const mbedtls_x509_name *b ) FLPURE;

    // Verify that `parent` signed `child` (returns 0 on success)
    int x509_crt_check_signature( const mbedtls_x509_crt *child,
                                  mbedtls_x509_crt *parent,
                                  mbedtls_x509_crt_restart_ctx *rs_ctx );

}
