//
// mbedSnippets.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "mbedSnippets.hh"
#include <cstring>

namespace litecore::crypto {

    // ======== The source code below is adapted from `x509_crt.c` in mbedTLS:

    /*
     *  X.509 certificate parsing and verification
     *
     *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
     *  SPDX-License-Identifier: Apache-2.0
     *
     *  Licensed under the Apache License, Version 2.0 (the "License"); you may
     *  not use this file except in compliance with the License.
     *  You may obtain a copy of the License at
     *
     *  http://www.apache.org/licenses/LICENSE-2.0
     *
     *  Unless required by applicable law or agreed to in writing, software
     *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
     *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
     *  See the License for the specific language governing permissions and
     *  limitations under the License.
     *
     *  This file is part of mbed TLS (https://tls.mbed.org)
     */


    /*
     * Like memcmp, but case-insensitive and always returns -1 if different
     */
    static int x509_memcasecmp(const void* s1, const void* s2, size_t len) {
        size_t        i;
        unsigned char diff;
        auto *        n1 = (const unsigned char*)s1, *n2 = (const unsigned char*)s2;

        for ( i = 0; i < len; i++ ) {
            diff = n1[i] ^ n2[i];

            if ( diff == 0 ) continue;

            if ( diff == 32 && ((n1[i] >= 'a' && n1[i] <= 'z') || (n1[i] >= 'A' && n1[i] <= 'Z')) ) { continue; }

            return (-1);
        }

        return (0);
    }

    /*
     * Compare two X.509 strings, case-insensitive, and allowing for some encoding
     * variations (but not all).
     *
     * Return 0 if equal, -1 otherwise.
     */
    static int x509_string_cmp(const mbedtls_x509_buf* a, const mbedtls_x509_buf* b) {
        if ( a->tag == b->tag && a->len == b->len && memcmp(a->p, b->p, b->len) == 0 ) { return (0); }

        if ( (a->tag == MBEDTLS_ASN1_UTF8_STRING || a->tag == MBEDTLS_ASN1_PRINTABLE_STRING)
             && (b->tag == MBEDTLS_ASN1_UTF8_STRING || b->tag == MBEDTLS_ASN1_PRINTABLE_STRING) && a->len == b->len
             && x509_memcasecmp(a->p, b->p, b->len) == 0 ) {
            return (0);
        }

        return (-1);
    }

    /*
     * Compare two X.509 Names (aka rdnSequence).
     *
     * See RFC 5280 section 7.1, though we don't implement the whole algorithm:
     * we sometimes return unequal when the full algorithm would return equal,
     * but never the other way. (In particular, we don't do Unicode normalisation
     * or space folding.)
     *
     * Return 0 if equal, -1 otherwise.
     */
    int x509_name_cmp(const mbedtls_x509_name* a, const mbedtls_x509_name* b) {
        /* Avoid recursion, it might not be optimised by the compiler */
        while ( a != nullptr || b != nullptr ) {
            if ( a == nullptr || b == nullptr ) return (-1);

            /* type */
            if ( a->oid.tag != b->oid.tag || a->oid.len != b->oid.len || memcmp(a->oid.p, b->oid.p, b->oid.len) != 0 ) {
                return (-1);
            }

            /* value */
            if ( x509_string_cmp(&a->val, &b->val) != 0 ) return (-1);

            /* structure of the list of sets */
            if ( a->private_next_merged != b->private_next_merged ) return (-1);

            a = a->next;
            b = b->next;
        }

        /* a == NULL == b */
        return (0);
    }

    /*
     * Check the signature of a certificate by its parent
     */
    int x509_crt_check_signature(const mbedtls_x509_crt* child, mbedtls_x509_crt* parent,
                                 mbedtls_x509_crt_restart_ctx* rs_ctx) {
        unsigned char hash[MBEDTLS_MD_MAX_SIZE];
        size_t        hash_len;
#if !defined(MBEDTLS_USE_PSA_CRYPTO)
        const mbedtls_md_info_t* md_info;
        md_info  = mbedtls_md_info_from_type(child->private_sig_md);
        hash_len = mbedtls_md_get_size(md_info);

        /* Note: hash errors can happen only after an internal error */
        if ( mbedtls_md(md_info, child->tbs.p, child->tbs.len, hash) != 0 ) return (-1);
#else
        psa_hash_operation_t hash_operation = PSA_HASH_OPERATION_INIT;
        psa_algorithm_t      hash_alg       = mbedtls_psa_translate_md(child->sig_md);

        if ( psa_hash_setup(&hash_operation, hash_alg) != PSA_SUCCESS ) return (-1);

        if ( psa_hash_update(&hash_operation, child->tbs.p, child->tbs.len) != PSA_SUCCESS ) { return (-1); }

        if ( psa_hash_finish(&hash_operation, hash, sizeof(hash), &hash_len) != PSA_SUCCESS ) { return (-1); }
#endif /* MBEDTLS_USE_PSA_CRYPTO */
        /* Skip expensive computation on obvious mismatch */
        if ( !mbedtls_pk_can_do(&parent->pk, child->private_sig_pk) ) return (-1);

#if defined(MBEDTLS_ECDSA_C) && defined(MBEDTLS_ECP_RESTARTABLE)
        if ( rs_ctx != NULL && child->sig_pk == MBEDTLS_PK_ECDSA ) {
            return (mbedtls_pk_verify_restartable(&parent->pk, child->sig_md, hash, hash_len, child->sig.p,
                                                  child->sig.len, &rs_ctx->pk));
        }
#else
        (void)rs_ctx;
#endif

        return (mbedtls_pk_verify_ext(child->private_sig_pk, child->private_sig_opts, &parent->pk,
                                      child->private_sig_md, hash, hash_len, child->private_sig.p,
                                      child->private_sig.len));
    }

}  // namespace litecore::crypto
