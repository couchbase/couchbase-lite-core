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

    /** An X.509 certificate. */
    class Cert : public KeyOwner {
    public:

        static constexpr unsigned kOneYear = 31536000;

        /** Parameters relating to the certificate subject, used when self-signing or requesting. */
        struct SubjectParameters {
            std::string subject_name;               // subject name for certificate (see below)
            uint8_t key_usage {0};                  // key usage flags (MBEDTLS_X509_KU_*)
            uint8_t ns_cert_type {0};               // Netscape flags (MBEDTLS_X509_NS_CERT_TYPE_*)

            SubjectParameters(const std::string &subjectName)
            :subject_name(subjectName) { }
            SubjectParameters(const char *subjectName NONNULL)
            :subject_name(subjectName) { }
        };

        /** Parameters for signing a certificate, used when self-signing or signing a request. */
        struct IssuerParameters {
            unsigned validity_secs {kOneYear};      // how long till expiration, starting now
            std::string serial {"1"};               // serial number string
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

        /** Instantiates a Cert from DER- or PEM-encoded certificate data. */
        explicit Cert(fleece::slice data);

        /** Creates and self-signs a certificate with the given options. */
        Cert(const SubjectParameters&,
             const IssuerParameters&,
             PrivateKey *keyPair NONNULL);

        /** Loads a certificate from persistent storage with the given subject public key. */
        static fleece::Retained<Cert> load(PublicKey*);

        /** The certificate's data. */
        fleece::alloc_slice data(KeyFormat =KeyFormat::DER);

        std::string subjectName();
        std::string info(const char *indent ="");

        /** The subject's public key. */
        fleece::Retained<PublicKey> subjectPublicKey()          {return new PublicKey(this);}

        /** Makes the certificate persistent by adding it to the platform-specific store
            (e.g. the Keychain on Apple devices.) */
        void makePersistent();

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
        /** Loads the private key from persistent storage, if available. */
        fleece::Retained<PersistentPrivateKey> loadPrivateKey();
#endif
        
        struct ::mbedtls_x509_crt* context()                    {return _cert.get();}

    private:
        friend class CertSigningRequest;

        Cert();
        ~Cert();
        static fleece::alloc_slice create(const SubjectParameters&,
                                          PublicKey *subjectKey NONNULL,
                                          const IssuerParameters&,
                                          PrivateKey *issuerKeyPair NONNULL,
                                          Cert *issuerCert =nullptr);
        virtual struct ::mbedtls_pk_context* keyContext() override;

        std::unique_ptr<struct ::mbedtls_x509_crt> _cert;
    };



    /** A certificate with its matching private key. */
    struct Identity : public fleece::RefCounted {
        Identity(Cert* NONNULL, PrivateKey* NONNULL);

        fleece::Retained<Cert> const        cert;
        fleece::Retained<PrivateKey> const  privateKey;
    };



    /** A request for a certificate, containing the subject's name and public key,
        to be sent to a Certificate Authority that will sign it. */
    class CertSigningRequest : public KeyOwner {
    public:
        /** Creates a Certificate Signing Request, to be sent to a CA that will sign it. */
        CertSigningRequest(const Cert::SubjectParameters &params, PrivateKey *subjectKey);

        /** Instantiates a request from pre-encoded DER or PEM data. */
        explicit CertSigningRequest(fleece::slice data);

        /** The encoded request data. */
        fleece::alloc_slice data(KeyFormat =KeyFormat::DER);

        /** The public key specified in the request. */
        fleece::Retained<PublicKey> subjectPublicKey()          {return new PublicKey(this);}

        /** The Subject Name specified in the request. */
        std::string subjectName();

        /** Signs the request, returning the completed Cert. */
        fleece::Retained<Cert> sign(const Cert::IssuerParameters&,
                                    PrivateKey *issuerKeyPair NONNULL,
                                    Cert *issuerCert =nullptr);

    protected:
        CertSigningRequest();
        ~CertSigningRequest();
        virtual struct ::mbedtls_pk_context* keyContext() override;

    private:
        static fleece::alloc_slice create(const Cert::SubjectParameters&, PrivateKey *subjectKey);
        struct ::mbedtls_x509_csr* context()                    {return _csr.get();}

        std::unique_ptr<struct ::mbedtls_x509_csr> _csr;
        fleece::alloc_slice _data;
    };

} }
