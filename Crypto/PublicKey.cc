//
// PublicKey.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "mbedtls/config.h"
#include "PublicKey.hh"
#include "TLSContext.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "mbedUtils.hh"
#include "Error.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/asn1.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#pragma clang diagnostic pop

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;
    using namespace net;

    LogDomain TLSLogDomain("TLS");


    Key::Key()                     {_pk = new mbedtls_pk_context; mbedtls_pk_init(_pk);}
    Key::~Key()                    {if (!_owner) {mbedtls_pk_free(_pk); delete _pk;}}

    string Key::description() {
        return format("%zd-bit %s %s key", mbedtls_pk_get_bitlen(_pk), mbedtls_pk_get_name(_pk),
                      (isPrivate() ? "private" : "public"));
    }


    alloc_slice Key::publicKeyDERData() {
        return allocDER(4096, [&](uint8_t *buf, size_t size) {
            return mbedtls_pk_write_pubkey_der(_pk, buf, size);
        });
    }

    alloc_slice Key::publicKeyRawData() {
        return allocDER(4096, [&](uint8_t *buf, size_t size) {
            auto pos = buf + size;
            return mbedtls_pk_write_pubkey(&pos, buf, _pk);
        });
    }

    alloc_slice Key::publicKeyData(KeyFormat format) {
        switch (format) {
            case KeyFormat::DER:
            case KeyFormat::PEM: {
                auto result = publicKeyDERData();
                if (format == KeyFormat::PEM)
                    result = convertToPEM(result, "PUBLIC KEY");
                return result;
            }
            case KeyFormat::Raw:
                return publicKeyRawData();
            default:
                Assert(false, "Invalid key format received (%d)", (int)format);
        }
    }


    string Key::digestString() {
        SHA1 digest(publicKeyData(KeyFormat::Raw));
        return slice(digest).hexString();
    }


    PublicKey::PublicKey(slice data) {
        parsePEMorDER(data, "public key", context(), &mbedtls_pk_parse_public_key);
    }


    PrivateKey::PrivateKey(slice data, slice password) {
        if (password.size == 0)
            password = nullslice; // interpret empty password as 'no password'
        parsePEMorDER(data, "private key", [&](const uint8_t* bytes, size_t size) {
            return mbedtls_pk_parse_key(context(), bytes, size,
                                        (const uint8_t*)password.buf, password.size);
        });
    }


    Retained<PrivateKey> PrivateKey::generateTemporaryRSA(unsigned keySizeInBits) {
        Retained<PrivateKey> key = new PrivateKey();
        auto ctx = key->context();
        TRY( mbedtls_pk_setup(ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) );
        LogTo(TLSLogDomain, "Generating %u-bit RSA key-pair...", keySizeInBits);
        TRY( mbedtls_rsa_gen_key(mbedtls_pk_rsa(*ctx),
                                 mbedtls_ctr_drbg_random, RandomNumberContext(),
                                 keySizeInBits, 65537) );
        return key;
    }


    alloc_slice PrivateKey::privateKeyData(KeyFormat format) {
        switch (format) {
            case KeyFormat::DER:
            case KeyFormat::PEM: {
                auto result = allocDER(4096, [&](uint8_t *buf, size_t size) {
                    return mbedtls_pk_write_key_der(context(), buf, size);
                });
                if (format == KeyFormat::PEM) {
                    string msg = litecore::format("%s PRIVATE KEY", mbedtls_pk_get_name(context()));
                    result = convertToPEM(result, msg.c_str());
                }
                return result;
            }
            case KeyFormat::Raw:
                return publicKeyRawData();
            default:
                Assert(false, "Invalid key format received (%d)", (int)format);
        }

    }


    ExternalPrivateKey::ExternalPrivateKey(unsigned keySizeInBits)
    :_keyLength( (keySizeInBits + 7) / 8)
    {
        // mbedTLS's "RSA-alt" feature lets you create a public key (mbedtls_pk_context) whose
        // operations delegate to custom callbacks. Here we create one that calls the
        // _decrypt, _sign, and publicKeyRawData methods, all of which are implemented by the
        // platform-specific subclass.

        auto decryptFunc = [](void *ctx, int mode, size_t *olen,
                              const unsigned char *input, unsigned char *output,
                              size_t output_max_len ) -> int {
            return ((ExternalPrivateKey*)ctx)->_decrypt(input, output, output_max_len, olen);
        };

        auto signFunc = [](void *ctx,
                           int (*f_rng)(void *, unsigned char *, size_t), void *p_rng,
                           int mode, mbedtls_md_type_t md_alg, unsigned int hashlen,
                           const unsigned char *hash, unsigned char *sig ) -> int {
            return ((ExternalPrivateKey*)ctx)->_sign(md_alg, slice(hash, hashlen), sig);
        };

        auto keyLengthFunc = []( void *ctx ) -> size_t {
            return ((ExternalPrivateKey*)ctx)->_keyLength;
        };

        auto writeKeyFunc = [](void *ctx, uint8_t **p, uint8_t *start) -> int {
            try {
                alloc_slice keyData = ((ExternalPrivateKey*)ctx)->publicKeyRawData();
                if (keyData.size > *p - start)
                    return MBEDTLS_ERR_ASN1_BUF_TOO_SMALL;
                memcpy(*p - keyData.size, keyData.buf, keyData.size);
                *p -= keyData.size;
                return int(keyData.size);
            } catch (const std::exception &x) {
                LogWarn(TLSLogDomain, "Unable to get data of public key: %s", x.what());
                return MBEDTLS_ERR_PK_FILE_IO_ERROR;
            }
        };

        TRY( mbedtls_pk_setup_rsa_alt2(context(), this,
                                       decryptFunc, signFunc, keyLengthFunc, writeKeyFunc) );
    }


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
#if 0
    // NOTE: These factory functions are implemented in a per-platform source file such as
    // PublicKey+Apple.mm, because they need to call platform-specific APIs.

    Retained<KeyPair> PersistentPrivateKey::generateRSA(unsigned keySizeInBits) {
        ... platform specific code...
    }

    Retained<KeyPair> PersistentPrivateKey::withPersistentID(const string &id) {
        ... platform specific code...
    }

    Retained<KeyPair> PersistentPrivateKey::withPublicKey(PublicKey*) {
        ... platform specific code...
    }
#endif
#endif // PERSISTENT_PRIVATE_KEY_AVAILABLE

} }
