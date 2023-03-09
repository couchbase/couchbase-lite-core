//
// PublicKey.hh
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
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include "SecureDigest.hh"
#include "Logging.hh"
#include <string>

struct mbedtls_pk_context;

namespace litecore { namespace crypto {
    class Cert;

    extern LogDomain TLSLogDomain;

    class KeyOwner : public fleece::RefCounted {
      public:
        virtual struct mbedtls_pk_context* keyContext() = 0;
    };


    enum class KeyFormat {
        Raw = -1,  ///< Raw key data; don't use unless you know what you're doing
        DER = 0,   ///< Binary PKCS1 format (an ASN.1 SubjectPublicKeyInfo)
        PEM = 1,   ///< ASCII encoding of DER
    };

    /** A public key. */
    class Key : public fleece::RefCounted {
      public:
        struct ::mbedtls_pk_context* context() { return _pk; }

        virtual bool isPrivate() = 0;

        bool isRSA() { return true; }  //TODO: Change when/if we support ECC

        std::string description();

        fleece::alloc_slice publicKeyData(KeyFormat = KeyFormat::DER);

        std::string digestString();

      protected:
        explicit Key(KeyOwner* owner) : _owner(owner), _pk(owner->keyContext()) {}

        Key();
        ~Key();
        virtual fleece::alloc_slice publicKeyDERData();
        virtual fleece::alloc_slice publicKeyRawData();

      private:
        fleece::Retained<KeyOwner>   _owner;
        struct ::mbedtls_pk_context* _pk;
    };

    class PublicKey final : public Key {
      public:
        /** Instantiates a PublicKey from data in DER (PKCS1) or PEM format. */
        explicit PublicKey(fleece::slice data);

        fleece::alloc_slice data(KeyFormat f = KeyFormat::DER) { return Key::publicKeyData(f); }

        virtual bool isPrivate() override { return false; }

      protected:
        friend class CertBase;

        PublicKey() = default;

        explicit PublicKey(KeyOwner* owner) : Key(owner) {}
    };


    class PersistentPrivateKey;

    /** An asymmetric key-pair. Usually the PersistentPrivateKey subclass is used. */
    class PrivateKey : public Key {
      public:
        /** Instantiates a PublicKey from data in PKCS1 or PKCS12 format. */
        explicit PrivateKey(fleece::slice data, fleece::slice password = {});

        /** Creates an in-memory key-pair for temporary use. Mostly useful only for testing,
            since the private key is exposed and difficult to save securely. */
        static fleece::Retained<PrivateKey> generateTemporaryRSA(unsigned keySizeInBits);

        virtual bool isPrivate() override { return true; }

        virtual PersistentPrivateKey* asPersistent() { return nullptr; }

        /** The private key's data. This will fail if the key is persistent, since its data is
            locked away in secure storage. */
        fleece::alloc_slice privateKeyData(KeyFormat format = KeyFormat::DER);

        /** Is the private key data accessible? I.e. will \ref privateKeyData return non-null?
            \ref PersistentPrivateKey overrides this to return false, since the key is locked
            in secure storage. */
        virtual bool isPrivateKeyDataAvailable() { return true; }

        /** The public key. */
        fleece::Retained<PublicKey> publicKey() { return new PublicKey(publicKeyData(KeyFormat::Raw)); }

      protected:
        PrivateKey() = default;
    };

    /** A key-pair stored externally; subclasses must implement the crypto operations. */
    class ExternalPrivateKey : public PrivateKey {
      protected:
        ExternalPrivateKey(unsigned keySizeInBits);

        /** Subclass must provide the public key data on request. */
        virtual fleece::alloc_slice publicKeyDERData() override = 0;

        /** Subclass-specific decryption implementation, using PKCS1 padding.
            @param input  The encrypted data; length is equal to _keyLength.
            @param output  Where to write the decrypted data.
            @param output_max_len  Maximum length of output to write; if there's not enough room,
                        return MBEDTLS_ERR_RSA_OUTPUT_TOO_LARGE.
            @param output_len  Store the actual decrypted data length here.
            @return  0 on success, or an mbedTLS error code on failure. */
        virtual int _decrypt(const void* input, void* output, size_t output_max_len, size_t* output_len) noexcept = 0;

        /** Subclass-specific signature implementation.
            @param digestAlgorithm  What type of digest to perform before signing.
            @param inputData  The data to be signed.
            @param outSignature  Write the signature here; length must be _keyLength.
            @return  0 on success, or an mbedTLS error code on failure. */
        virtual int _sign(int /*mbedtls_md_type_t*/ digestAlgorithm, fleece::slice inputData,
                          void* outSignature) noexcept = 0;

        /** Key length, in _bytes_ not bits. */
        unsigned const _keyLength;
    };


#if !defined(PERSISTENT_PRIVATE_KEY_AVAILABLE) && defined(__APPLE__)
#    define PERSISTENT_PRIVATE_KEY_AVAILABLE
#endif

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE

    /** An asymmetric key-pair, with persistent storage in a secure container. The type of
        container is platform-specific; for example, on iOS and macOS it is the Keychain.
        This class is abstract, with platform-specific subclasses managing the actual storage. */
    class PersistentPrivateKey : public ExternalPrivateKey {
      public:
        /** Generates a new RSA key-pair. The key-pair is stored persistently (e.g. in the
            iOS / macOS Keychain) associated with the given label. */
        static fleece::Retained<PersistentPrivateKey> generateRSA(unsigned keySizeInBits);

        /** Loads an existing stored key-pair that matches the given public key. */
        static fleece::Retained<PersistentPrivateKey> withPublicKey(PublicKey* NONNULL);

        /** Loads an existing stored key-pair that matches the given cert's public key. */
        static fleece::Retained<PersistentPrivateKey> withCertificate(Cert* NONNULL);

        /** Permanently removes the key-pair from storage.
            Don't make any more calls to this object afterwards. */
        virtual void remove() = 0;

        virtual PersistentPrivateKey* asPersistent() override { return this; }

      protected:
        PersistentPrivateKey(unsigned keySizeInBits) : ExternalPrivateKey(keySizeInBits) {}
    };

#endif  // PERSISTENT_PRIVATE_KEY_AVAILABLE

}}  // namespace litecore::crypto
