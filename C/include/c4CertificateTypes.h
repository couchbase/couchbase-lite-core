//
// c4CertificateTypes.h
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

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup certificates Certificates
    @{ */

/** Certificate usage types. A certificate may have one or more of these. */
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


/** Certificate subject name attributes, from RFC 4519 and RFC 5280 (sec. 4.2.1.6)
    Only the CommonName is required; it's used as the visible name of the certificate.
    If the cert is to be used for a TLS server, the CommonName must match its DNS name. */
typedef C4Slice C4CertNameAttributeID;

// Some common Distinguished Name attributes:
#define kC4Cert_CommonName        C4STR("CN")           // e.g. "Jane Doe", (or "jane.example.com")
#define kC4Cert_Pseudonym         C4STR("pseudonym")    // e.g. "plainjane837"
#define kC4Cert_GivenName         C4STR("GN")           // e.g. "Jane"
#define kC4Cert_Surname           C4STR("SN")           // e.g. "Doe"
#define kC4Cert_Organization      C4STR("O")            // e.g. "Example Corp."
#define kC4Cert_OrganizationUnit  C4STR("OU")           // e.g. "Marketing"
#define kC4Cert_PostalAddress     C4STR("postalAddress")// e.g. "123 Example Blvd #2A"
#define kC4Cert_Locality          C4STR("locality")     // e.g. "Boston"
#define kC4Cert_PostalCode        C4STR("postalCode")   // e.g. "02134"
#define kC4Cert_StateOrProvince   C4STR("ST")           // e.g. "Massachusetts" (or "Quebec", ...)
#define kC4Cert_Country           C4STR("C")            // e.g. "us" (2-letter ISO country code)

// These are the Subject Alternative Name attributes:
#define kC4Cert_EmailAddress      C4STR("rfc822Name")   // 'rfc822Name', e.g. "jane@example.com"
#define kC4Cert_Hostname          C4STR("dNSName")      // 'dnsName', e.g. "www.example.com"
#define kC4Cert_URL               C4STR("uniformResourceIdentifier") // "https://example.com/jane"
#define kC4Cert_IPAddress         C4STR("iPAddress")    // *Binary* IP address, e.g. "\x0A\x00\x01\x01"
#define kC4Cert_RegisteredID      C4STR("registeredID") // Some sort of domain-specific ID?

/** Information about a single component of a certificate's subject name. */
typedef struct C4CertNameInfo {
    C4StringResult id;      ///< X.509 attribute name (e.g. "CN" or "O"), like a C4CertNameAttributeID
    C4StringResult value;   ///< The value of the name component, i.e. the name.
} C4CertNameInfo;


/** \name Certificate Requests and Signing
 @{ */

/** A component of an X.509 "Relative Distinguished Name" or "Subject Alternative Name". */
typedef struct {
    C4CertNameAttributeID attributeID;  ///< Attribute name, e.g. "CN" or "O"
    C4String value;                     ///< Value of the attribute
} C4CertNameComponent;

/** Parameters for signing a certificate. These will be used by the Certificate Authority
    (CA), which might be the same as the subject if self-signing. */
typedef struct {
    unsigned validityInSeconds;         ///< seconds from signing till expiration (default 1yr)
    C4String serialNumber;              ///< serial number string (default "1")
    int maxPathLen;                     ///< maximum CA path length (default -1, meaning none)
    bool isCA;                          ///< will this be a CA certificate? (default false)
    bool addAuthorityIdentifier;        ///< add authority identifier to cert? (default true)
    bool addSubjectIdentifier;          ///< add subject identifier to cert? (default true)
    bool addBasicConstraints;           ///< add basic constraints extension? (default true)
} C4CertIssuerParameters;

/** Default issuer parameters. Every C4CertIssuerParameters should be initialized from this. */
CBL_CORE_API extern const C4CertIssuerParameters kDefaultCertIssuerParameters;

/** @} */


/** \name Key-Pairs
 @{ */

/** Supported key-pair algorithms. */
typedef C4_ENUM(uint8_t, C4KeyPairAlgorithm) {
    kC4RSA,
};
/** @} */


/** \name Externally-Implemented Key-Pairs
 @{ */

/** Digest algorithms to be used when generating signatures.
    (Note: These enum values match mbedTLS's `mbedtls_md_type_t`.) */
typedef C4_ENUM(int, C4SignatureDigestAlgorithm) {
    kC4SignatureDigestNone = 0,  ///< No digest, just direct signature of input data.
    kC4SignatureDigestSHA1 = 4,  ///< SHA-1 message digest.
    kC4SignatureDigestSHA224,    ///< SHA-224 message digest.
    kC4SignatureDigestSHA256,    ///< SHA-256 message digest.
    kC4SignatureDigestSHA384,    ///< SHA-384 message digest.
    kC4SignatureDigestSHA512,    ///< SHA-512 message digest.
    kC4SignatureDigestRIPEMD160, ///< RIPEMD-160 message digest.
};


/** Callbacks that must be provided to create an external key; these perform the crypto operations. */
typedef struct C4ExternalKeyCallbacks {
    /** Provides the _public_ key's raw data, as an ASN.1 DER sequence of [modulus, exponent].
        @param externalKey  The client-provided key token given to c4keypair_fromExternal.
        @param output  Where to copy the key data.
        @param outputMaxLen  Maximum length of output that can be written.
        @param outputLen  Store the length of the output here before returning.
        @return  True on success, false on failure. */
    bool (*publicKeyData)(void *externalKey,
                          void *output,
                          size_t outputMaxLen,
                          size_t *outputLen);
    /** Decrypts data using the private key.
        @param externalKey  The client-provided key token given to c4keypair_fromExternal.
        @param input  The encrypted data (size is always equal to the key size.)
        @param output  Where to write the decrypted data.
        @param outputMaxLen  Maximum length of output that can be written.
        @param outputLen  Store the length of the output here before returning.
        @return  True on success, false on failure. */
    bool (*decrypt)(void *externalKey,
                    C4Slice input,
                    void *output,
                    size_t outputMaxLen,
                    size_t *outputLen);
    /** Uses the private key to generate a signature of input data.
        @param externalKey  The client-provided key value given to c4keypair_fromExternal.
        @param digestAlgorithm  Indicates what type of digest to create the signature from.
        @param inputData  The data to be signed.
        @param outputSignature  Write the signature here; length must be equal to the key size.
        @return  True on success, false on failure.
        \note The data in inputData is _already hashed_ and does not need to be hashed by the caller.  The
              algorithm is provided as a reference for what was used to perform the hashing.    */
    bool (*sign)(void *externalKey,
                 C4SignatureDigestAlgorithm digestAlgorithm,
                 C4Slice inputData,
                 void *outSignature);
    /** Called when the C4KeyPair is released and the externalKey is no longer needed, so that
        your code can free any associated resources. (This callback is optionaly and may be NULL.)
        @param externalKey  The client-provided key value given when the C4KeyPair was created. */
    void (* C4NULLABLE free)(void *externalKey);
} C4ExternalKeyCallbacks;

/** @} */


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END

#endif // COUCHBASE_ENTERPRISE
