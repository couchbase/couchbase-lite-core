//
// Certificate.hh
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
#include "PublicKey.hh"
#include "fleece/function_ref.hh"
#include <initializer_list>
#include <memory>
#include <optional>
#include <time.h>
#include <utility>
#include <vector>

struct mbedtls_x509_crt;
struct mbedtls_x509_csr;
struct mbedtls_asn1_sequence;

namespace litecore { namespace crypto {

    /** An X.509 Distinguished Name encoded as a string in LDAP format. */
    class DistinguishedName : public fleece::alloc_slice {
      public:
        struct Entry {
            fleece::slice key;    // LDAP attribute like "CN", "O", etc (as supported by mbedTLS)
            fleece::slice value;  // Value of the attribute
        };

        /** Creates a subjectName from a list of key/value strings. */
        DistinguishedName(const Entry *begin NONNULL, const Entry *end NONNULL);

        DistinguishedName(const std::vector<Entry> &);

        DistinguishedName(std::initializer_list<Entry> entries)
            : DistinguishedName(std::vector<Entry>(entries.begin(), entries.end())) {}

        explicit DistinguishedName(fleece::alloc_slice s) : alloc_slice(s) {}

        explicit DistinguishedName(fleece::slice s) : alloc_slice(s) {}

        using VectorForm = std::vector<std::pair<fleece::slice, fleece::alloc_slice>>;

        VectorForm asVector();

        fleece::alloc_slice operator[](fleece::slice key);

      private:
        friend class Cert;
        friend class CertSigningRequest;
    };


    /** X.509 tag values for a Subject Alternative Name */
    enum class SANTag : uint8_t {
        kOtherName = 0,  // these byte values are defined as part of X.509v3
        kRFC822Name,
        kDNSName,
        kX400AddressName,
        kDirectoryName,
        kEDIPartyName,
        kURIName,
        kIPAddress,
        kRegisteredID,
    };

    using SubjectAltName = std::pair<SANTag, fleece::alloc_slice>;

    /** An X.509 Subject Alternative Name entry. */
    class SubjectAltNames : public std::vector<SubjectAltName> {
      public:
        using Tag = SANTag;

        static std::optional<Tag> tagNamed(fleece::slice name);
        static fleece::slice      nameOfTag(Tag);

        SubjectAltNames() = default;
        explicit SubjectAltNames(::mbedtls_asn1_sequence *);

        fleece::alloc_slice encode() const;

        fleece::alloc_slice operator[](Tag) const;

        const SubjectAltName &operator[](size_t i) const { return vector<SubjectAltName>::operator[](i); }
    };

    enum NSCertType : uint8_t {
        SSL_CLIENT        = 0x80,  // these byte values are defined as part NS's cert extensions
        SSL_SERVER        = 0x40,
        EMAIL             = 0x20,
        OBJECT_SIGNING    = 0x10,
        RESERVED          = 0x08,
        SSL_CA            = 0x04,
        EMAIL_CA          = 0x02,
        OBJECT_SIGNING_CA = 0x01,
    };

    /** Abstract superclass of Cert and CertRequest. */
    class CertBase : public KeyOwner {
      public:
        static constexpr unsigned kOneYear = 31536000;

        /** Parameters relating to the certificate subject, used when self-signing or requesting. */
        struct SubjectParameters {
            DistinguishedName subjectName;      // Identity info for certificate (see below)
            SubjectAltNames   subjectAltNames;  // More identity info
            unsigned          keyUsage{0};      // key usage flags (MBEDTLS_X509_KU_*)
            NSCertType        nsCertType{0};    // Netscape flags (MBEDTLS_X509_NS_CERT_TYPE_*)

            SubjectParameters(DistinguishedName dn) : subjectName(dn) {}
        };

        /** Parameters for signing a certificate, used when self-signing or signing a request. */
        struct IssuerParameters {
            unsigned            validity_secs{kOneYear};         // how long till expiration, starting now
            fleece::alloc_slice serial{"1"};                     // serial number string
            int                 max_pathlen{-1};                 // maximum CA path length (-1 for none)
            bool                is_ca{false};                    // is this a CA certificate?
            bool                add_authority_identifier{true};  // add authority identifier to cert?
            bool                add_subject_identifier{true};    // add subject identifier to cert?
            bool                add_basic_constraints{true};     // add basic constraints extension to cert?
        };

        // subjectName is a "Relative Distinguished Name" represented as a series of KEY=VALUE
        // pairs separated by commas. The keys are defined by LDAP and listed in RFC4519. The ones
        // recognized by mbedTLS (see x509_create.c) include:
        //      commonName (aka CN), pseudonym, emailAddress, postalAddress,
        //      locality (aka L), stateOrProvinceName (ST), country (C),
        //      organization (O), organizationalUnitName (OU)

        /** The certificate's data. */
        virtual fleece::alloc_slice data(KeyFormat = KeyFormat::DER);

        virtual bool isSigned() { return false; }

        virtual fleece::alloc_slice summary(const char *indent = "");

        virtual DistinguishedName subjectName()     = 0;
        virtual unsigned          keyUsage()        = 0;
        virtual NSCertType        nsCertType()      = 0;
        virtual SubjectAltNames   subjectAltNames() = 0;

        /** The subject's public key. */
        fleece::Retained<PublicKey> subjectPublicKey() { return new PublicKey(this); }

      protected:
        virtual fleece::slice derData()                                                = 0;
        virtual int           writeInfo(char *buf, size_t bufSize, const char *indent) = 0;
    };

    /** A signed X.509 certificate. */
    class Cert final : public CertBase {
      public:
        /** Instantiates a Cert from DER- or PEM-encoded certificate data.
            \note PEM data may contain multiple certs, forming a chain. If so, you can find the
                next cert in the chain by calling the \ref next method. */
        explicit Cert(fleece::slice data);

        /** Creates and self-signs a certificate with the given options. */
        Cert(const SubjectParameters &, const IssuerParameters &, PrivateKey *keyPair NONNULL);

        /** Loads a certificate from persistent storage with the given subject public key. */
        static fleece::Retained<Cert> load(PublicKey *);

        /** Check if a certificate with the given subject public key exists in the persistent storage. */
        static bool exists(PublicKey *);

        virtual bool isSigned() override { return true; }

        bool                        isSelfSigned();
        DistinguishedName           subjectName() override;
        unsigned                    keyUsage() override;
        NSCertType                  nsCertType() override;
        SubjectAltNames             subjectAltNames() override;
        virtual fleece::alloc_slice summary(const char *indent = "") override;

        /** Returns the cert's creation and expiration times. */
        std::pair<time_t, time_t> validTimespan();

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
        /** Save the certificate chain to a persistent key store with the persistent ID */
        void save(const std::string &persistentID, bool entireChain);

        /** Load the certificate chain from a persistent key store with the persistent ID */
        static fleece::Retained<Cert> loadCert(const std::string &persistentID);

        /** Check if a certificate with the given persistent ID exists in the persistent key store */
        static bool exists(const std::string &persistentID);

        /** Delete the certificate chain with the persistent ID */
        static void deleteCert(const std::string &persistentID);

        /** Loads the private key from persistent storage, if available. */
        fleece::Retained<PersistentPrivateKey> loadPrivateKey();
#endif

        struct ::mbedtls_x509_crt *context() { return _cert; }

        //---- Certificate chains

        /** Returns true if there are following certs in a chain. */
        bool hasChain();

        /** Returns the next certificate in the chain, if any. */
        fleece::Retained<Cert> next();

        /** Appends a cert to the end of the chain. */
        void append(Cert *NONNULL);

        /** Converts the entire chain into a series of certs in PEM format. */
        fleece::alloc_slice dataOfChain();

        //---- Root certificates

#ifdef __APPLE__
#    define ROOT_CERT_LOOKUP_AVAILABLE
#endif

#ifdef ROOT_CERT_LOOKUP_AVAILABLE
        /** Returns the trusted root certificate that signed this cert, if any. */
        fleece::Retained<Cert> findSigningRootCert();
#endif

      protected:
        virtual fleece::slice                derData() override;
        virtual int                          writeInfo(char *buf, size_t bufSize, const char *indent) override;
        virtual struct ::mbedtls_pk_context *keyContext() override;

      private:
        friend class CertSigningRequest;

        Cert(Cert *prev NONNULL, mbedtls_x509_crt *);
        ~Cert();
        static fleece::alloc_slice create(const SubjectParameters &, PublicKey *subjectKey    NONNULL,
                                          const IssuerParameters &, PrivateKey *issuerKeyPair NONNULL,
                                          Cert *issuerCert = nullptr);

        struct ::mbedtls_x509_crt *_cert;           // mbedTLS parsed cert object
        fleece::Retained<Cert>     _prev;           // Previous Cert in chain (strong ref)
        Cert                      *_next{nullptr};  // Next Cert in chain (weak ref)
    };

    /** A certificate with its matching private key. */
    struct Identity : public fleece::RefCounted {
        Identity(Cert *NONNULL, PrivateKey *NONNULL);

        fleece::Retained<Cert> const       cert;
        fleece::Retained<PrivateKey> const privateKey;
    };

    /** A request for an X.509 certificate, containing the subject's name and public key,
        to be sent to a Certificate Authority that will sign it. */
    class CertSigningRequest final : public CertBase {
      public:
        /** Creates a Certificate Signing Request, to be sent to a CA that will sign it. */
        CertSigningRequest(const Cert::SubjectParameters &params, PrivateKey *subjectKey);

        /** Instantiates a request from pre-encoded DER or PEM data. */
        explicit CertSigningRequest(fleece::slice data);

        /** The Subject Name specified in the request. */
        DistinguishedName subjectName() override;

        unsigned        keyUsage() override;
        NSCertType      nsCertType() override;
        SubjectAltNames subjectAltNames() override;

        /** Signs the request, returning the completed Cert. */
        fleece::Retained<Cert> sign(const Cert::IssuerParameters &, PrivateKey *issuerKeyPair NONNULL,
                                    Cert *issuerCert = nullptr);

      protected:
        CertSigningRequest();
        ~CertSigningRequest();
        virtual struct ::mbedtls_pk_context *keyContext() override;
        virtual fleece::slice                derData() override;
        virtual int                          writeInfo(char *buf, size_t bufSize, const char *indent) override;

      private:
        static fleece::alloc_slice create(const Cert::SubjectParameters &, PrivateKey *subjectKey);

        struct ::mbedtls_x509_csr *context() { return _csr.get(); }

        std::unique_ptr<struct ::mbedtls_x509_csr> _csr;
    };

}}  // namespace litecore::crypto
