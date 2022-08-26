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
#include <time.h>

#ifdef COUCHBASE_ENTERPRISE

#ifdef __cplusplus
extern "C" {
#endif

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

    /** \name Certificate and CSR Functions
     @{ */

    /** Instantiates a C4Cert from X.509 certificate data in DER or PEM form.
        \note PEM data might consist of a series of certificates. If so, the returned C4Cert
              will represent only the first, and you can iterate over the next by calling
              \ref c4cert_nextInChain.
        \note You are responsible for releasing the returned reference. */
    C4Cert* c4cert_fromData(C4Slice certData,
                            C4Error *outError) C4API;

    /** Returns the encoded X.509 data in DER (binary) or PEM (ASCII) form.
        \warning DER format can only encode a _single_ certificate, so if this C4Cert includes
            multiple certificates, use PEM format to preserve them.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4cert_copyData(C4Cert* C4NONNULL,
                                  bool pemEncoded) C4API;

    /** Returns a human-readable, multi-line string describing the certificate in detail.
        \note You are responsible for releasing the returned data. */
    C4StringResult c4cert_summary(C4Cert* C4NONNULL) C4API;

    /** Returns the cert's Subject Name, which identifies the cert's owner.
        This is an X.509 structured string consisting of "KEY=VALUE" pairs separated by commas,
        where the keys are attribute names. (Commas in values are backslash-escaped.)
        \note Rather than parsing this yourself, use \ref c4cert_subjectNameComponent.
        \note You are responsible for releasing the returned data. */
    C4StringResult c4cert_subjectName(C4Cert* C4NONNULL) C4API;

    /** Returns one component of a cert's subject name, given the attribute ID.
        \note If there are multiple names with this ID, only the first is returned.
        \note You are responsible for releasing the returned string. */
    C4StringResult c4cert_subjectNameComponent(C4Cert* C4NONNULL, C4CertNameAttributeID) C4API;

    /** Information about a single component of a certificate's subject name. */
    typedef struct C4CertNameInfo {
        C4StringResult id;      ///< X.509 attribute name (e.g. "CN" or "O"), like a C4CertNameAttributeID
        C4StringResult value;   ///< The value of the name component, i.e. the name.
    } C4CertNameInfo;

    /** Returns one component of a cert's subject name, given a zero-based index into the list.
        If the index is out of range, it returns a null slice.
        @param cert  The certificate to examine.
        @param index  Zero-based index into the subject name list.
        @param outInfo  The component's name and value will be written here.
        @return  True if the index was valid, false if out of range.
        \note You are responsible for releasing `outInfo->value`. */
    bool c4cert_subjectNameAtIndex(C4Cert* cert C4NONNULL,
                                   unsigned index,
                                   C4CertNameInfo *outInfo C4NONNULL) C4API;

    /** Returns the time range during which a (signed) certificate is valid.
        @param cert  The signed certificate.
        @param outCreated  On return, the date/time the cert became valid (was signed).
        @param outExpires  On return, the date/time at which the certificate expires. */
    void c4cert_getValidTimespan(C4Cert* cert C4NONNULL,
                                 C4Timestamp *outCreated,
                                 C4Timestamp *outExpires);

    /** Returns the usage flags of a cert. */
    C4CertUsage c4cert_usages(C4Cert* C4NONNULL) C4API;

    /** Returns true if the issuer is the same as the subject.
        \note This will be true of root CA certs, as well as self-signed peer certs. */
    bool c4cert_isSelfSigned(C4Cert* C4NONNULL) C4API;

    /** Returns a certificate's public key.
        \note You are responsible for releasing the returned key reference. */
    C4KeyPair* c4cert_getPublicKey(C4Cert* C4NONNULL) C4API;

    /** Loads a certificate's matching private key from the OS's persistent store, if it exists,
        and returns the key-pair with both private and public key.
        \note You are responsible for releasing the returned key reference. */
    C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert* C4NONNULL,
                                           C4Error *outError) C4API;

    /** @} */



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


    /** Creates a Certificate Signing Request, i.e. an unsigned certificate.
        \note You are responsible for releasing the returned reference.
        @param nameComponents  Pointer to an array of one or more \ref C4CertNameComponent structs.
        @param nameCount  Number of items in the \p nameComponents array.
        @param certUsages  Flags giving intended usage. (The certificate will be rejected by peers
                if you try to use it for something not specified in its certUsages!)
        @param subjectKey  The owner's private key that this certificate will attest to.
        @param outError  On failure, the error info will be stored here.
        @return  The new certificate request, or NULL on failure. */
    C4Cert* c4cert_createRequest(const C4CertNameComponent *nameComponents C4NONNULL,
                                 size_t nameCount,
                                 C4CertUsage certUsages,
                                 C4KeyPair *subjectKey C4NONNULL,
                                 C4Error *outError) C4API;

    /** Instantiates a C4Cert from an X.509 certificate signing request (CSR) in DER or PEM form.
        \note You are responsible for releasing the returned reference. */
    C4Cert* c4cert_requestFromData(C4Slice certRequestData,
                                   C4Error *outError) C4API;

    /** Returns true if this is a signed certificate, false if it's a signing request (CSR). */
    bool c4cert_isSigned(C4Cert* C4NONNULL) C4API;

    /** Completion routine called when an async \ref c4cert_sendSigningRequest finishes.
        @param context  The same `context` value passed to \ref c4cert_sendSigningRequest.
        @param signedCert  The signed certificate, if the operation was successful, else NULL.
        @param error  The error, if the operation failed. */
    typedef void (*C4CertSigningCallback)(void *context,
                                          C4Cert *signedCert,
                                          C4Error error);

    /** Sends an unsigned certificate (a CSR) to a Certificate Authority (CA) over HTTP
        to be signed, and _asynchronously_ returns the signed certificate.
        \note There is no standard protocol for sending CSRs; this function uses the protocol
                defined by Cloudflare's CFSSL.
        @param certRequest  The certificate request to be signed.
        @param address  The URL of the CA server.
        @param optionsDictFleece  Network options, just like the corresponding field in
                    \ref C4ReplicatorParameters. Most importantly, this is used to specify
                    authentication, without which the CA server won't sign anything.
        @param callback A function that will be called, on a background thread, after the request
                    completes.
        @param context  An arbitrary value that will be passed to the callback function.
        @param outError If the parameters are invalid, error info will be written here.
        @return  True if the parameters are valid and the request will be sent; else false. */
    bool c4cert_sendSigningRequest(C4Cert *certRequest C4NONNULL,
                                   C4Address address,
                                   C4Slice optionsDictFleece,
                                   C4CertSigningCallback callback C4NONNULL,
                                   void *context,
                                   C4Error *outError) C4API;

    /** Signs an unsigned certificate (a CSR) with a private key, and returns the new signed
        certificate. This is the primary function of a Certificate Authority; but it can also
        be used to create self-signed certificates.
        \note You are responsible for releasing the returned reference.
        @param certRequest  The unsigned certificate to be signed.
        @param params  Capabilities to store in the cert; if NULL, uses defaults.
        @param issuerPrivateKey  The Certificate Authority's private key. (If self-signing a
                    cert, this should be the same as the `subjectKey` it was created with.)
        @param issuerCert  The Certificate Authority's certificate (which must match
                    \p issuerPrivateKey), or NULL if self-signing.
        @param outError  On failure, the error info will be stored here.
        @return  The signed certificate, or NULL on failure. */
    C4Cert* c4cert_signRequest(C4Cert *certRequest C4NONNULL,
                               const C4CertIssuerParameters *params,
                               C4KeyPair *issuerPrivateKey C4NONNULL,
                               C4Cert *issuerCert,
                               C4Error *outError) C4API;

    /** @} */



    /** \name Certificate Chains
     @{ */

    /** Returns the next certificate in the chain after this one, if any.
        \note You are responsible for releasing the returned reference. */
    C4Cert* c4cert_nextInChain(C4Cert* C4NONNULL) C4API;

    /** Returns the encoded data of this cert and the following ones in the chain, in PEM form.
        \note You are responsible for releasing the returned data.*/
    C4SliceResult c4cert_copyChainData(C4Cert* C4NONNULL) C4API;

    /** @} */



    /** \name Certificate Persistence
     @{ */

    /** Saves a certificate to a persistent storage for easy lookup by name, or deletes a saved cert.
        \note The certificate needs to be signed to save into the storage.
        @param cert  The certificate to store, or NULL to delete any saved cert with that name.
        @param entireChain  True if the entire cert chain should be saved.
        @param name  The name to save as.
        @param outError  On failure, the error info will be stored here.
        @return  True on success, false on failure. */
    bool c4cert_save(C4Cert *cert,
                     bool entireChain,
                     C4String name,
                     C4Error *outError);

    /** Loads a certificate from a persistent storage given the name it was saved under.
        \note You are responsible for releasing the returned key reference.
        @param name  The name the certificate was saved with.
        @param outError  On failure, the error info will be stored here.
        @return  The certificate, or NULL if missing or if it failed to parse. */
    C4Cert* c4cert_load(C4String name,
                        C4Error *outError);

    bool c4cert_exists(C4String name,
                       C4Error *outError);

    /** @} */



    /** \name Key-Pairs
     @{ */

    /** Supported key-pair algorithms. */
    typedef C4_ENUM(uint8_t, C4KeyPairAlgorithm) {
        kC4RSA,
    };


    /** Creates a new key-pair.
        \note You are responsible for releasing the returned reference.
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
    C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm,
                                  unsigned sizeInBits,
                                  bool persistent,
                                  C4Error *outError) C4API;

    /** Loads a public key from its data.
        The resulting C4KeyPair will not have a private key.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_fromPublicKeyData(C4Slice publicKeyData,
                                           C4Error *outError) C4API;

    /** Loads a private key from its data.
        The resulting C4KeyPair will have both a public and private key.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice privateKeyData,
                                            C4Slice passwordOrNull,
                                            C4Error *outError) C4API;

    /** Returns true if the C4KeyPair has a private as well as a public key. */
    bool c4keypair_hasPrivateKey(C4KeyPair* C4NONNULL) C4API;

    /** Returns a hex digest of the public key.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_publicKeyDigest(C4KeyPair* C4NONNULL) C4API;

    /** Returns the public key data.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_publicKeyData(C4KeyPair* C4NONNULL) C4API;

    /** Returns the private key data, if the private key is known and its data is accessible.
        \note Persistent private keys generally don't have accessible data.
        \note You are responsible for releasing the returned data. */
    C4SliceResult c4keypair_privateKeyData(C4KeyPair* C4NONNULL) C4API;

    /** Returns true if the C4KeyPair is stored int the OS's persistent store. */
    bool c4keypair_isPersistent(C4KeyPair* C4NONNULL) C4API;

    /** Attempts to find & load the persistent key-pair matching this public key.
        \note If there is no matching persistent key, returns NULL but sets no error.
        \note You are responsible for releasing the returned reference. */
    C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair* C4NONNULL,
                                                 C4Error *outError) C4API;

    /** Removes a private key from persistent storage. */
    bool c4keypair_removePersistent(C4KeyPair* C4NONNULL,
                                C4Error *outError) C4API;

    /** @} */



    /** \name Externally-Implemented Key-Pairs
     @{ */

    struct C4ExternalKeyCallbacks;

    /** Creates a C4KeyPair object that wraps an external key-pair managed by client code.
        Signatures and decryption will be performed by calling client-defined callbacks.
        @param algorithm  The type of key (currently only RSA.)
        @param keySizeInBits  The key size, measured in bits, e.g. 2048.
        @param externalKey  An abitrary token that will be passed to the callbacks; most likely a
                            pointer to your own key structure.
        @param callbacks  A struct containing callback functions to do the work.
        @param outError  On failure, the error info will be stored here.
        @return  The key object, or NULL on failure. */
    C4KeyPair* c4keypair_fromExternal(C4KeyPairAlgorithm algorithm,
                                      size_t keySizeInBits,
                                      void *externalKey,
                                      struct C4ExternalKeyCallbacks callbacks,
                                      C4Error *outError);


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
        void (*free)(void *externalKey);
    } C4ExternalKeyCallbacks;


    /** @} */

#ifdef __cplusplus
}
#endif

#endif // COUCHBASE_ENTERPRISE
