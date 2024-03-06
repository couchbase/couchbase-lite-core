//
// c4Certificate.h
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

#include "c4CertificateTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup certificates Certificates
    @{ */

/** \name Certificate and CSR Functions
 @{ */

/** Returns the time range during which a (signed) certificate is valid.
    @param cert  The signed certificate.
    @param outCreated  On return, the date/time the cert became valid (was signed).
    @param outExpires  On return, the date/time at which the certificate expires. */
CBL_CORE_API void c4cert_getValidTimespan(C4Cert* cert, C4Timestamp* C4NULLABLE outCreated,
                                          C4Timestamp* C4NULLABLE outExpires);

#ifdef COUCHBASE_ENTERPRISE

/** Instantiates a C4Cert from X.509 certificate data in DER or PEM form.
    \note PEM data might consist of a series of certificates. If so, the returned C4Cert
          will represent only the first, and you can iterate over the next by calling
          \ref c4cert_nextInChain.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4Cert* c4cert_fromData(C4Slice certData, C4Error* C4NULLABLE outError) C4API;

/** Returns the encoded X.509 data in DER (binary) or PEM (ASCII) form.
    \warning DER format can only encode a _single_ certificate, so if this C4Cert includes
        multiple certificates, use PEM format to preserve them.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4SliceResult c4cert_copyData(C4Cert*, bool pemEncoded) C4API;

/** Returns a human-readable, multi-line string describing the certificate in detail.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4StringResult c4cert_summary(C4Cert*) C4API;

/** Returns the cert's Subject Name, which identifies the cert's owner.
    This is an X.509 structured string consisting of "KEY=VALUE" pairs separated by commas,
    where the keys are attribute names. (Commas in values are backslash-escaped.)
    \note Rather than parsing this yourself, use \ref c4cert_subjectNameComponent.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4StringResult c4cert_subjectName(C4Cert*) C4API;

/** Returns one component of a cert's subject name, given the attribute ID.
    \note If there are multiple names with this ID, only the first is returned.
    \note You are responsible for releasing the returned string. */
CBL_CORE_API C4StringResult c4cert_subjectNameComponent(C4Cert*, C4CertNameAttributeID) C4API;

/** Returns one component of a cert's subject name, given a zero-based index into the list.
    If the index is out of range, it returns a null slice.
    @param cert  The certificate to examine.
    @param index  Zero-based index into the subject name list.
    @param outInfo  The component's name and value will be written here.
    @return  True if the index was valid, false if out of range.
    \note You are responsible for releasing `outInfo->value`. */
NODISCARD CBL_CORE_API bool c4cert_subjectNameAtIndex(C4Cert* cert, unsigned index, C4CertNameInfo* outInfo) C4API;


/** Returns the usage flags of a cert. */
CBL_CORE_API C4CertUsage c4cert_usages(C4Cert*) C4API;

/** Returns true if the issuer is the same as the subject.
    \note This will be true of root CA certs, as well as self-signed peer certs. */
CBL_CORE_API bool c4cert_isSelfSigned(C4Cert*) C4API;

/** Returns a certificate's public key.
    \note You are responsible for releasing the returned key reference. */
NODISCARD CBL_CORE_API C4KeyPair* c4cert_getPublicKey(C4Cert*) C4API;

/** Loads a certificate's matching private key from the OS's persistent store, if it exists,
    and returns the key-pair with both private and public key.
    \note You are responsible for releasing the returned key reference. */
NODISCARD CBL_CORE_API C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert*, C4Error* C4NULLABLE outError) C4API;

/** @} */


/** \name Certificate Requests and Signing
 @{ */

/** Creates a Certificate Signing Request, i.e. an unsigned certificate.
    \note You are responsible for releasing the returned reference.
    @param nameComponents  Pointer to an array of one or more \ref C4CertNameComponent structs.
    @param nameCount  Number of items in the \p nameComponents array.
    @param certUsages  Flags giving intended usage. (The certificate will be rejected by peers
            if you try to use it for something not specified in its certUsages!)
    @param subjectKey  The owner's private key that this certificate will attest to.
    @param outError  On failure, the error info will be stored here.
    @return  The new certificate request, or NULL on failure. */
NODISCARD CBL_CORE_API C4Cert* c4cert_createRequest(const C4CertNameComponent* nameComponents, size_t nameCount,
                                                    C4CertUsage certUsages, C4KeyPair* subjectKey,
                                                    C4Error* C4NULLABLE outError) C4API;

/** Instantiates a C4Cert from an X.509 certificate signing request (CSR) in DER or PEM form.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4Cert* c4cert_requestFromData(C4Slice certRequestData, C4Error* C4NULLABLE outError) C4API;

/** Returns true if this is a signed certificate, false if it's a signing request (CSR). */
CBL_CORE_API bool c4cert_isSigned(C4Cert*) C4API;

/** Completion routine called when an async \ref c4cert_sendSigningRequest finishes.
    @param context  The same `context` value passed to \ref c4cert_sendSigningRequest.
    @param signedCert  The signed certificate, if the operation was successful, else NULL.
    @param error  The error, if the operation failed. */
typedef void (*C4CertSigningCallback)(void* context, C4Cert* signedCert, C4Error error);

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
NODISCARD CBL_CORE_API bool c4cert_sendSigningRequest(C4Cert* certRequest, C4Address address, C4Slice optionsDictFleece,
                                                      C4CertSigningCallback callback, void* C4NULLABLE context,
                                                      C4Error* C4NULLABLE outError) C4API;

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
NODISCARD CBL_CORE_API C4Cert* c4cert_signRequest(C4Cert* certRequest, const C4CertIssuerParameters* C4NULLABLE params,
                                                  C4KeyPair* issuerPrivateKey, C4Cert* C4NULLABLE issuerCert,
                                                  C4Error* C4NULLABLE outError) C4API;

/** @} */


/** \name Certificate Chains
 @{ */

/** Returns the next certificate in the chain after this one, if any.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4Cert* C4NULLABLE c4cert_nextInChain(C4Cert*) C4API;

/** Returns the encoded data of this cert and the following ones in the chain, in PEM form.
    \note You are responsible for releasing the returned data.*/
CBL_CORE_API C4SliceResult c4cert_copyChainData(C4Cert*) C4API;

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
NODISCARD CBL_CORE_API bool c4cert_save(C4Cert* C4NULLABLE cert, bool entireChain, C4String name,
                                        C4Error* C4NULLABLE outError);

/** Loads a certificate from a persistent storage given the name it was saved under.
    \note You are responsible for releasing the returned key reference.
    @param name  The name the certificate was saved with.
    @param outError  On failure, the error info will be stored here.
    @return  The certificate, or NULL if missing or if it failed to parse. */
NODISCARD CBL_CORE_API C4Cert* c4cert_load(C4String name, C4Error* C4NULLABLE outError);

/** Check if a certificate with the given name exists in the persistent keystore.
 * @param name The name the certificate was saved with.
 * @param outError On failure, the error info will be stored here.
 * @return true if the certificate exists, otherwise false.
 */
NODISCARD CBL_CORE_API bool c4cert_exists(C4String name, C4Error* C4NULLABLE outError);

/** @} */


/** \name Key-Pairs
 @{ */

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
NODISCARD CBL_CORE_API C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm, unsigned sizeInBits, bool persistent,
                                                     C4Error* C4NULLABLE outError) C4API;

/** Loads a public key from its data.
    The resulting C4KeyPair will not have a private key.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4KeyPair* c4keypair_fromPublicKeyData(C4Slice             publicKeyData,
                                                              C4Error* C4NULLABLE outError) C4API;

/** Loads a private key from its data.
    The resulting C4KeyPair will have both a public and private key.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice privateKeyData, C4Slice passwordOrNull,
                                                               C4Error* C4NULLABLE outError) C4API;

/** Returns true if the C4KeyPair has a private as well as a public key. */
CBL_CORE_API bool c4keypair_hasPrivateKey(C4KeyPair*) C4API;

/** Returns a hex digest of the public key.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4SliceResult c4keypair_publicKeyDigest(C4KeyPair*) C4API;

/** Returns the public key data.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4SliceResult c4keypair_publicKeyData(C4KeyPair*) C4API;

/** Returns the private key data, if the private key is known and its data is accessible.
    \note Persistent private keys generally don't have accessible data.
    \note You are responsible for releasing the returned data. */
CBL_CORE_API C4SliceResult c4keypair_privateKeyData(C4KeyPair*) C4API;

/** Returns true if the C4KeyPair is stored int the OS's persistent store. */
CBL_CORE_API bool c4keypair_isPersistent(C4KeyPair*) C4API;

/** Attempts to find & load the persistent key-pair matching this public key.
    \note If there is no matching persistent key, returns NULL but sets no error.
    \note You are responsible for releasing the returned reference. */
NODISCARD CBL_CORE_API C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair*, C4Error* C4NULLABLE outError) C4API;

/** Removes a private key from persistent storage. */
NODISCARD CBL_CORE_API bool c4keypair_removePersistent(C4KeyPair*, C4Error* C4NULLABLE outError) C4API;

/** @} */


/** \name Externally-Implemented Key-Pairs
 @{ */

/** Creates a C4KeyPair object that wraps an external key-pair managed by client code.
    Signatures and decryption will be performed by calling client-defined callbacks.
    @param algorithm  The type of key (currently only RSA.)
    @param keySizeInBits  The key size, measured in bits, e.g. 2048.
    @param externalKey  An abitrary token that will be passed to the callbacks; most likely a
                        pointer to your own key structure.
    @param callbacks  A struct containing callback functions to do the work.
    @param outError  On failure, the error info will be stored here.
    @return  The key object, or NULL on failure. */
NODISCARD CBL_CORE_API C4KeyPair* c4keypair_fromExternal(C4KeyPairAlgorithm algorithm, size_t keySizeInBits,
                                                         void* externalKey, C4ExternalKeyCallbacks callbacks,
                                                         C4Error* C4NULLABLE outError);

/** @} */

/** @} */

#endif  // COUCHBASE_ENTERPRISE

C4API_END_DECLS
C4_ASSUME_NONNULL_END
