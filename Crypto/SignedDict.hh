//
// SignedDict.hh
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "Signing.hh"
#include "fleece/Fleece.hh"

namespace litecore::crypto {

    /// Possible results of verifying a signature.
    /// Any result other than `Valid` means the signature is not valid and the contents of the
    /// object are not to be trusted. The specific values might help in choosing an error message.
    enum class VerifyResult {
        Valid,              ///< The signature is valid!
        Expired,            ///< The signature was valid but has expired (or isn't valid yet.)
        MissingKey,         ///< No key was given and there's no key embedded in the signature.
        ConflictingKeys,    ///< Key given doesn't match public key embedded in signature.
        InvalidProperties,  ///< Properties in the signature dict are missing or invalid.
        InvalidDigest,      ///< Digest in signature doesn't match that of the signed object itself.
        InvalidSignature    ///< The signature data itself didn't check out.
    };


    /// Creates a signature of a Fleece Value, usually a Dict.
    /// The signature takes the form of a Dict.
    /// @param toBeSigned  The Fleece value, usually a Dict, to be signed.
    /// @param key  A private key to sign with, RSA or Ed25519.
    /// @param expirationTimeMinutes  How long until the signature expires. Units are **minutes**.
    ///                               Default value is one year.
    /// @param embedPublicKey  If true, the public key data will be included in the signature object.
    ///                        If false it's omitted; then whoever verifies the signature
    ///                        must already know the public key through some other means.
    /// @param otherMetadata  An optional Dict of other properties to add to the signature Dict.
    ///                       These properties will be signed, so any tampering of them will
    ///                       invalidate the signature just like tampering with `toBeSigned`.
    /// @return  The signature object, a (mutable) Dict.
    [[nodiscard]]
    fleece::MutableDict makeSignature(fleece::Value toBeSigned,
                                      const SigningKey &key,
                                      int64_t expirationTimeMinutes = 60 * 24 * 365,
                                      bool embedPublicKey = true,
                                      fleece::Dict otherMetadata =nullptr);


    /// Returns the public key embedded in a signature, if there is one.
    /// Returns `nullptr` if the signature has no key data for any known algorithm.
    /// Throws `error::CryptoError` if the key data exists but is invalid.
    unique_ptr<VerifyingKey> getSignaturePublicKey(fleece::Dict signature);


    /// Returns the public key, with the given algorithm, embedded in a signature.
    /// Returns `nullptr` if the signature has no key data for that algorithm.
    /// Throws `error::CryptoError` if the key data exists but is invalid.
    unique_ptr<VerifyingKey> getSignaturePublicKey(fleece::Dict signature,
                                                   const char *algorithmName);


    /// Verifies a signature of `document` using the signature object `signature`.
    /// The `document` must be _exactly the same_ as when it was signed; any properties added to it
    /// afterwards need to be removed. This probably includes the `signature` itself!
    /// @param toBeVerified  The Fleece value which is to be verified.
    /// @param signature  The signature. (Must not be contained in `toBeVerified`!)
    /// @param publicKey  The `VerifyingKey` matching the `SigningKey` that made the signature.
    ///                   If `nullptr`, a key embedded in the signature will be used.
    /// @return  An status value, which will be `Valid` if the signature is valid;
    ///          or `MissingDigest` or `MissingKey` if no digest or key properties corresponding to
    ///          the verifier were found;
    ///          or other values if the signature itself is invalid or expired.
    [[nodiscard]]
    VerifyResult verifySignature(fleece::Value toBeVerified,
                                 fleece::Dict signature,
                                 const VerifyingKey *publicKey =nullptr);
}
