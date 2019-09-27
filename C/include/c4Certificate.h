//
// c4Certificate.h
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
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

#pragma once

#include "c4Base.h"

#ifdef COUCHBASE_ENTERPRISE

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup certificates Certificates
        @{ */

    /** An X.509 certificate, or certificate signing request (CSR). */
    typedef struct C4Cert C4Cert;

    /** An asymmetric key or key-pair (RSA, etc.)*/
    typedef struct C4Key C4Key;


    /** Certificate types. A certificate may be of one or more of these.*/
    typedef C4_OPTIONS(uint8_t, C4CertUsage) { // Note: Same values as `MBEDTLS_X509_NS_CERT_TYPE_*`
        kC4CertUsage_NotSpecified      = 0x00, ///< No specified usage (not generally useful)
        kC4CertUsage_TLSClient         = 0x80, ///< TLS (SSL) client cert
        kC4CertUsage_TLSServer         = 0x40, ///< TLS (SSL) server cert
        kC4CertUsage_Email             = 0x20, ///< Email signing and encryption
        kC4CertUsage_ObjectSigning     = 0x10, ///< Signing arbitrary data
        kC4CertUsage_TLS_CA            = 0x04, ///< CA for signing TLS cert requests
        kC4CertUsage_Email_CA          = 0x02, ///< CA for signing email cert requests
        kC4CertUsage_ObjectSigning_CA  = 0x01  ///< CA for signing object-signing cert requests
    };


    /** Parameters for signing a certificate. These will be used by the Certificate Authority
        (CA), which might be the same as the subject if self-signing. */
    typedef struct {
        unsigned validityInSeconds;         ///< seconds from signing till expiration (default 1 year)
        C4String serialNumber;              ///< serial number string (default "1")
        int maxPathLen;                     ///< maximum CA path length (default -1, meaning none)
        bool isCA;                          ///< will this be a CA certificate? (default false)
        bool addAuthorityIdentifier;        ///< add authority identifier to cert? (default true)
        bool addSubjectIdentifier;          ///< add subject identifier to cert? (default true)
        bool addBasicConstraints;           ///< add basic constraints extension? (default true)
    } C4CertIssuerParameters;

    /** Default issuer parameters. Every C4CertIssuerParameters should be initialized from this. */
    extern const C4CertIssuerParameters kDefaultCertIssuerParameters;


    /** \name Certificate and CSR Functions
     @{ */

    /** Creates a Certificate Signing Request, i.e. an unsigned certificate.
        @param subjectName Structured name of the certificate owner. This is an X.509
                "Relative Distinguished Name" represented as a series of `KEY=VALUE` pairs
                separated by commas. The keys are defined by LDAP and listed in RFC4519.
                The ones recognized by LiteCore (actually mbedTLS) include:
                "commonName" (aka "CN"), "pseudonym", "emailAddress", "postalAddress",
                "locality" (aka "L"), "stateOrProvinceName" ("ST"), "country" ("C"),
                "organization" ("O"), "organizationalUnitName" ("OU").
                Example: "CN=Buffy Summers,emailAddress=buffy@example.com,L=Sunnydale,ST=CA,C=US"
        @param certUsages  Flags giving intended usage. (The certificate will be rejected by peers
                if you try to use it for something not specified in its certUsages!)
        @param subjectKey  The owner's private key that this certificate will attest to.
        @param outError  On failure, the error info will be stored here.
        @return  The new certificate request, or NULL on failure. */
    C4Cert* c4cert_createRequest(C4String subjectName,
                                 C4CertUsage certUsages,
                                 C4Key *subjectKey C4NONNULL,
                                 C4Error *outError) C4API;

    /** Instantiates a C4Cert from X.509 certificate data in DER or PEM form. */
    C4Cert* c4cert_fromData(C4Slice certData) C4API;

    /** Returns the encoded X.509 data in DER form. */
    C4SliceResult c4cert_copyData(C4Cert* C4NONNULL) C4API;

    /** Returns the cert's subject name. */
    C4StringResult c4cert_subjectName(C4Cert* C4NONNULL) C4API;

    /** Returns a human-readable string describing the certificate in detail. */
    C4StringResult c4cert_summary(C4Cert* C4NONNULL) C4API;

    /** Returns true if this is a signed certificate, false if it's a signing request (CSR). */
    bool c4cert_isSigned(C4Cert* C4NONNULL) C4API;

    /** Signs an unsigned certificate (a CSR.)
        @param cert  The unsigned certificate to be signed.
        @param params  Capabilities to store in the cert; if NULL, uses defaults.
        @param issuerPrivateKey  The Certificate Authority's private key. (If self-signing a
                    cert, this should be the same as the `subjectKey` it was created with.)
        @param outError  On failure, the error info will be stored here.
        @return  The signed certificate, or NULL on failure. */
    C4Cert* c4cert_signRequest(C4Cert *cert C4NONNULL,
                               const C4CertIssuerParameters *params,
                               C4Key *issuerPrivateKey C4NONNULL,
                               C4Error *outError) C4API;

    /** Saves a certificate to the OS's persistent store, if there is one. */
    bool c4cert_makePersistent(C4Cert* C4NONNULL,
                               C4Error *outError);

    /** Returns a certificate's public key.
        You are responsible for releasing the returned key reference. */
    C4Key* c4cert_getPublicKey(C4Cert* C4NONNULL) C4API;

    /** Loads a certificate's matching private key from the OS's persistent store, if it exists.
        You are responsible for releasing the returned key reference. */
    C4Key* c4cert_loadPersistentPrivateKey(C4Cert* C4NONNULL,
                                           C4Error *outError) C4API;

    /** Retains a C4Cert, incrementing its reference count. */
    static inline C4Cert* c4cert_retain(C4Cert* cert) C4API {return (C4Cert*) c4base_retain(cert);}

    /** Releases a C4Cert reference. */
    static inline void c4cert_release(C4Cert* cert) C4API {c4base_release(cert);}


    /** @} */



    /** \name Key-Pairs
     @{ */

    /** Supported key-pair algorithms. */
    typedef C4_ENUM(uint8_t, C4KeyPairAlgorithm) {
        kC4RSA,
    };


    /** Creates a new key-pair.
        \warning Key-pairs should usually be persistent. This is more secure because the private
                 key data is extremely difficult to access. A non-persistent key-pair's private
                 key data lives in the process's heap, and if you store it yourself it's difficult
                 to do so securely.
        @param algorithm  The type of key to create, e.g. RSA.
        @param sizeInBits  The size (length) of the key in bits. Larger sizes are more secure.
                            Available key sizes depend on the key type.
        @param persistent  True if the key should be managed by the OS's persistent store.
        @param outError  On failure, the error info will be stored here.
        @return  The new key, or NULL on failure. */
    C4Key* c4key_createPair(C4KeyPairAlgorithm algorithm,
                            unsigned sizeInBits,
                            bool persistent,
                            C4Error *outError) C4API;

    /** Loads a public key from its data.
        The resulting C4Key will not have a private key. */
    C4Key* c4key_fromPublicKeyData(C4Slice publicKeyData) C4API;

    /** Loads a private key from its data.
        The resulting C4Key will have both a public and private key. */
    C4Key* c4key_fromPrivateKeyData(C4Slice privateKeyData) C4API;

    /** Returns true if the C4Key has a private as well as a public key. */
    bool c4key_hasPrivateKey(C4Key* C4NONNULL) C4API;

    /** Returns true if the C4Key is stored int the OS's persistent store. */
    bool c4key_isPersistent(C4Key* C4NONNULL) C4API;

    /** Returns the public key data. */
    C4SliceResult c4key_publicKeyData(C4Key* C4NONNULL) C4API;

    /** Returns the private key data, if the private key is known and its data is accessible.
        \note Persistent private keys generally don't have accessible data. */
    C4SliceResult c4key_privateKeyData(C4Key* C4NONNULL) C4API;

    /** Retains a C4Key, incrementing its reference count. */
    static inline C4Key* c4key_retain(C4Key* key) C4API {return (C4Key*) c4base_retain(key);}

    /** Releases a C4Key reference. */
    static inline void c4key_release(C4Key* key) C4API {c4base_release(key);}

    /** @} */

#ifdef __cplusplus
}
#endif

#endif // COUCHBASE_ENTERPRISE
