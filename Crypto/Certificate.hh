//
// Certificate.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "PublicKey.hh"
#include <memory>
#include <time.h>

struct mbedtls_x509_crt;
struct mbedtls_x509_csr;

namespace litecore { namespace crypto {

    /** Abstract superclass of Cert and CertRequest. */
    class CertBase : public KeyOwner {
    public:
        static constexpr unsigned kOneYear = 31536000;

        /** Parameters relating to the certificate subject, used when self-signing or requesting. */
        struct SubjectParameters {
            fleece::alloc_slice subject_name;       // subject name for certificate (see below)
            uint8_t key_usage {0};                  // key usage flags (MBEDTLS_X509_KU_*)
            uint8_t ns_cert_type {0};               // Netscape flags (MBEDTLS_X509_NS_CERT_TYPE_*)

            SubjectParameters(fleece::slice subjectName) :subject_name(subjectName) { }
            SubjectParameters(fleece::alloc_slice subjectName) :subject_name(subjectName) { }
        };

        /** Parameters for signing a certificate, used when self-signing or signing a request. */
        struct IssuerParameters {
            unsigned validity_secs {kOneYear};      // how long till expiration, starting now
            fleece::alloc_slice serial {"1"};       // serial number string
            int max_pathlen {-1};                   // maximum CA path length (-1 for none)
            bool is_ca {false};                     // is this a CA certificate?
            bool add_authority_identifier {true};   // add authority identifier to cert?
            bool add_subject_identifier {true};     // add subject identifier to cert?
            bool add_basic_constraints {true};      // add basic constraints extension to cert?
        };

        // subject_name is a "Relative Distinguished Name" represented as a series of KEY=VALUE
        // pairs separated by commas. The keys are defined by LDAP and listed in RFC4519. The ones
        // recognized by mbedTLS (see x509_create.c) include:
        //      commonName (aka CN), pseudonym, emailAddress, postalAddress,
        //      locality (aka L), stateOrProvinceName (ST), country (C),
        //      organization (O), organizationalUnitName (OU)

        /** The certificate's data. */
        virtual fleece::alloc_slice data(KeyFormat =KeyFormat::DER);

        virtual bool isSigned()                         {return false;}

        fleece::alloc_slice summary(const char *indent ="");

        virtual fleece::alloc_slice subjectName() =0;

        /** The subject's public key. */
        fleece::Retained<PublicKey> subjectPublicKey()  {return new PublicKey(this);}

    protected:
        using ParseFn = int (*)(void*,const uint8_t*,size_t);
        void parseData(fleece::slice data, void *context, ParseFn parse);
        virtual fleece::slice derData() =0;
        virtual int writeInfo(char *buf, size_t bufSize, const char *indent) =0;
    };


    /** A signed X.509 certificate. */
    class Cert : public CertBase {
    public:

        /** Instantiates a Cert from DER- or PEM-encoded certificate data. */
        explicit Cert(fleece::slice data);

        /** Creates and self-signs a certificate with the given options. */
        Cert(const SubjectParameters&,
             const IssuerParameters&,
             PrivateKey *keyPair NONNULL);

        /** Loads a certificate from persistent storage with the given subject public key. */
        static fleece::Retained<Cert> load(PublicKey*);

        virtual bool isSigned() override                        {return true;}
        fleece::alloc_slice subjectName() override;

        /** Makes the certificate persistent by adding it to the platform-specific store
            (e.g. the Keychain on Apple devices.) */
        void makePersistent();

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
        /** Loads the private key from persistent storage, if available. */
        fleece::Retained<PersistentPrivateKey> loadPrivateKey();
#endif
        
        struct ::mbedtls_x509_crt* context()                    {return _cert.get();}

    protected:
        virtual fleece::slice derData() override;
        virtual int writeInfo(char *buf, size_t bufSize, const char *indent) override;
        virtual struct ::mbedtls_pk_context* keyContext() override;
    private:
        friend class CertSigningRequest;

        Cert();
        ~Cert();
        static fleece::alloc_slice create(const SubjectParameters&,
                                          PublicKey *subjectKey NONNULL,
                                          const IssuerParameters&,
                                          PrivateKey *issuerKeyPair NONNULL,
                                          Cert *issuerCert =nullptr);

        std::unique_ptr<struct ::mbedtls_x509_crt> _cert;
    };



    /** A certificate with its matching private key. */
    struct Identity : public fleece::RefCounted {
        Identity(Cert* NONNULL, PrivateKey* NONNULL);

        fleece::Retained<Cert> const        cert;
        fleece::Retained<PrivateKey> const  privateKey;
    };



    /** A request for an X.509 certificate, containing the subject's name and public key,
        to be sent to a Certificate Authority that will sign it. */
    class CertSigningRequest : public CertBase {
    public:
        /** Creates a Certificate Signing Request, to be sent to a CA that will sign it. */
        CertSigningRequest(const Cert::SubjectParameters &params, PrivateKey *subjectKey);

        /** Instantiates a request from pre-encoded DER or PEM data. */
        explicit CertSigningRequest(fleece::slice data);

        /** The Subject Name specified in the request. */
        fleece::alloc_slice subjectName() override;

        /** Signs the request, returning the completed Cert. */
        fleece::Retained<Cert> sign(const Cert::IssuerParameters&,
                                    PrivateKey *issuerKeyPair NONNULL,
                                    Cert *issuerCert =nullptr);

    protected:
        CertSigningRequest();
        ~CertSigningRequest();
        virtual struct ::mbedtls_pk_context* keyContext() override;
        virtual fleece::slice derData() override;
        virtual int writeInfo(char *buf, size_t bufSize, const char *indent) override;

    private:
        static fleece::alloc_slice create(const Cert::SubjectParameters&, PrivateKey *subjectKey);
        struct ::mbedtls_x509_csr* context()                    {return _csr.get();}

        std::unique_ptr<struct ::mbedtls_x509_csr> _csr;
    };

} }
