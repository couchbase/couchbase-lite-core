//
// PublicKey.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include "SecureDigest.hh"
#include <string>

struct mbedtls_pk_context;

namespace litecore { namespace crypto {
    class Cert;


    class KeyOwner : public fleece::RefCounted {
    public:
        virtual struct mbedtls_pk_context* keyContext() =0;
    };


    enum class KeyFormat {
        Raw = -1,          ///< Raw key data; don't use unless you know what you're doing
        DER = 0,           ///< Binary PKCS1 format (an ASN.1 SubjectPublicKeyInfo)
        PEM = 1,           ///< ASCII encoding of DER
    };


    /** A public key. */
    class Key : public fleece::RefCounted {
    public:
        struct ::mbedtls_pk_context* context()  {return _pk;}

        virtual bool isPrivate() =0;

        std::string description();

        fleece::alloc_slice publicKeyData(KeyFormat =KeyFormat::DER);

    protected:
        explicit Key(KeyOwner *owner)     :_owner(owner), _pk(owner->keyContext()) { }
        Key();
        ~Key();
        virtual fleece::alloc_slice publicKeyDERData();
        virtual fleece::alloc_slice publicKeyRawData();

    private:
        fleece::Retained<KeyOwner> _owner;
        struct ::mbedtls_pk_context* _pk;
    };


    
    class PublicKey : public Key {
    public:
        /** Instantiates a PublicKey from data in DER (PKCS1) or PEM format. */
        explicit PublicKey(fleece::slice data);

        fleece::alloc_slice data(KeyFormat f =KeyFormat::DER)   {return Key::publicKeyData(f);}

        virtual bool isPrivate() override       {return false;}

    protected:
        friend class Cert;
        friend class CertSigningRequest;
        
        PublicKey()                             { }
        explicit PublicKey(KeyOwner *owner)     :Key(owner) { }
    };



    /** An asymmetric key-pair. Usually the PersistentPrivateKey subclass is used. */
    class PrivateKey : public Key {
    public:
        /** Creates an in-memory key-pair for temporary use. Mostly useful only for testing,
            since the private key is exposed and difficult to save securely. */
        static fleece::Retained<PrivateKey> generateTemporaryRSA(unsigned keySizeInBits);

        virtual bool isPrivate() override               {return true;}

        /** The public key. */
        fleece::Retained<PublicKey> publicKey() {
            return new PublicKey(publicKeyData(KeyFormat::DER));
        }
    };



    /** An asymmetric key-pair, with persistent storage in a secure container. The type of
        container is platform-specific; for example, on iOS and macOS it is the Keychain.
        This class is abstract, with platform-specific subclasses managing the actual storage. */
    class PersistentPrivateKey : public PrivateKey {
    public:
        /** Generates a new RSA key-pair. The key-pair is stored persistently (e.g. in the
            iOS / macOS Keychain) associated with the given label. */
        static fleece::Retained<PersistentPrivateKey> generateRSA(unsigned keySizeInBits,
                                                                  const std::string &label);

        /** Loads an existing stored key-pair with the given label, or returns nullptr if none. */
        static fleece::Retained<PersistentPrivateKey> load(const std::string &label);

        /** Permanently removes the stored key-pair with the given label, or returns false if none. */
        static bool remove(const std::string &label);

        const std::string& label()                          {return _label;}

    protected:
        PersistentPrivateKey(unsigned keySizeInBits, const std::string &label);

        /** Platform-specific decryption implementation, using PKCS1 padding.
            @param input  The encrypted data; length is equal to _keyLength.
            @param output  Where to write the decrypted data.
            @param output_max_len  Maximum length of output to write; if there's not enough room,
                        return MBEDTLS_ERR_RSA_OUTPUT_TOO_LARGE.
            @param output_len  Store the actual decrypted data length here.
            @return  0 on success, or an mbedTLS error code on failure. */
        virtual int _decrypt(const void *input,
                             void *output,
                             size_t output_max_len,
                             size_t *output_len) noexcept =0;

        /** Platform-specific signature implementation.
            @param digestAlgorithm  What type of digest to perform before signing.
            @param inputData  The data to be signed.
            @param outSignature  Write the signature here; length must be _keyLength.
            @return  0 on success, or an mbedTLS error code on failure. */
        virtual int _sign(int/*mbedtls_md_type_t*/ digestAlgorithm,
                          fleece::slice inputData,
                          void *outSignature) noexcept =0;

        virtual fleece::alloc_slice publicKeyDERData() override =0;

        unsigned const _keyLength;              // In bytes, not bits!
        std::string const _label;
    };

} }
