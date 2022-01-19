//
// SignedDict.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SignedDict.hh"
#include "SecureDigest.hh"
#include "Error.hh"
#include "fleece/Mutable.hh"
#include "Base64.hh"

namespace litecore::crypto {
    using namespace std::string_literals;
    using namespace fleece;

    /*
     Signature dict schema:
     {
        "sig_RSA"
     or "sig_Ed25519":  A digital signature of the canonical JSON form of this signature
                        dict itself. (When verifying, this property must be removed
                        since it didn't exist when the signature was being computed.)
                        The suffix after "sig_" is the value of `SigningKey::algorithmName()`.
        "digest_SHA":   A SHA digest of the canonical JSON of the value being signed.
                        Usually SHA256; the specific algorithm can be determined by the data's size.
        "key":          The [optional] public key data for verifying the signature.
                        The algorithm is the same as indicated by the "sig_..." property's suffix.
                        If not present, the verifier must know the key through some other means
                        and pass it to `verifySignature()`.
        "date":         A timestamp of when the signature was created.
        "expires":      The number of minutes before the signature expires.
     }

     Other optional application-defined properties may be added to the signature dict.
     They become part of the signature, so they cannot be tampered with,
     but the signature verification code here doesn't pay any attention to them.

     - Data is either a base64-encoded string, or a Fleece data value.
     - A timestamp is either a number of milliseconds since the Unix epoch, or an ISO-8601 string.
     - Canonical JSON rules:
       * No whitespace.
       * Dicts are ordered by sorting the keys lexicographically (before encoding them as JSON.)
       * Strings use only the escape sequences `\\`, `\"`, `\r`, `\n`, `\t`, and the generic
         escape sequence `\uxxxx` for other control characters and 0x7F. All others are literal,
         including non-ASCII UTF-8 sequences.
       * No leading zeroes in integers, and no `-` in front of `0`.
       * Floating-point numbers should be avoided since there's no universally recognized algorithm
         to convert them to decimal, so different encoders may produce different results.
     */


    // The amount by which a signature's start date may be in the future and still be considered
    // valid when verifying it.
    // This compensates for clock inconsistency between computers: if you create a signature and
    // immediately send it over the network to someone else, but their system clock is slightly
    // behind yours, they will probably see the signature's date as being in the future. Without
    // some allowance for this, they'd reject the signature.
    // In other words, this is the maximum clock variance we allow when verifying a just-created
    // signature.
    static constexpr int64_t kClockDriftAllowanceMS = 60 * 1000;
    

    MutableDict makeSignature(Value toBeSigned,
                              const SigningKey &privateKey,
                              int64_t expirationTimeMinutes,
                              bool embedPublicKey,
                              Dict otherMetadata)
    {
        // Create a signature object containing the document digest and the public key:
        MutableDict signature = otherMetadata ? otherMetadata.mutableCopy() : MutableDict::newDict();
        SHA256 digest(toBeSigned.toJSON(false, true));
        signature["digest_SHA"].setData(digest);
        if (embedPublicKey)
            signature["key"].setData(privateKey.verifyingKeyData());
        if (expirationTimeMinutes > 0) {
            if (!signature["date"])
                signature["date"] = FLTimestamp_Now();// alloc_slice(FLTimestamp_ToString(FLTimestamp_Now(), false));
            if (!signature["expires"])
                signature["expires"] = expirationTimeMinutes;
        }

        // Sign the signature object, add the signature, and return it:
        alloc_slice signatureData = privateKey.sign(signature.toJSON(false, true));
        signature["sig_"s + privateKey.algorithmName()].setData(signatureData);
        return signature;
    }


    static alloc_slice convertToData(Value dataOrStr) {
        if (slice data = dataOrStr.asData(); data)
            return alloc_slice(data);
        else if (slice str = dataOrStr.asString(); str)
            return base64::decode(str);
        else
            return nullslice;
    }


    unique_ptr<VerifyingKey> getSignaturePublicKey(Dict signature, const char *algorithmName) {
        alloc_slice data = convertToData(signature["key"]);
        if (!data)
            return nullptr;
        if (!signature["sig_"s + algorithmName])
            return nullptr;
        return VerifyingKey::instantiate(data, algorithmName);
    }


    unique_ptr<VerifyingKey> getSignaturePublicKey(Dict signature) {
        auto key = getSignaturePublicKey(signature, kRSAAlgorithmName);
        if (!key)
            key = getSignaturePublicKey(signature, kEd25519AlgorithmName);
        return key;
    }


    VerifyResult verifySignature(Value toBeVerified,
                                 Dict signature,
                                 const VerifyingKey *publicKey)
    {
        // Get the digest property from the signature:
        Value digestVal = signature["digest_SHA"];
        if (!digestVal)
            return VerifyResult::InvalidProperties;
        auto digest = convertToData(digestVal);
        if (!digest || digest.size != sizeof(SHA256))
            return VerifyResult::InvalidProperties;

        unique_ptr<VerifyingKey> embeddedKey;
        if (publicKey) {
            // If there's an embedded key, make sure it matches the key I was given:
            if (Value key = signature["key"]; key && convertToData(key) != publicKey->data())
                return VerifyResult::ConflictingKeys;
        } else {
            // If no public key was given, read it from the signature:
            embeddedKey = getSignaturePublicKey(signature);
            if (!embeddedKey)
                return VerifyResult::MissingKey;
            publicKey = embeddedKey.get();
        }

        // Find the signature data itself:
        string sigProp = "sig_"s + publicKey->algorithmName();
        auto signatureData = convertToData(signature[sigProp]);
        if (!signatureData)
            return VerifyResult::InvalidProperties;

        // Generate canonical JSON of the signature dict, minus the "sig_" property:
        MutableDict strippedSignature = signature.mutableCopy();
        strippedSignature.remove(sigProp);
        alloc_slice signedData = strippedSignature.toJSON(false, true);

        // Verify the signature:
        if (!publicKey->verifySignature(signedData, signatureData))
            return VerifyResult::InvalidSignature;

        // Verify that the digest matches that of the document:
        if (digest != SHA256(toBeVerified.toJSON(false, true)).asSlice())
            return VerifyResult::InvalidDigest;

        // Verify that the signature is not expired nor not-yet-valid:
        if (Value date = signature["date"]; date) {
            FLTimestamp now = FLTimestamp_Now();
            FLTimestamp start = date.asTimestamp();
            if (start <= 0)
                return VerifyResult::InvalidProperties;
            if (now + kClockDriftAllowanceMS < start)
                return VerifyResult::Expired;
            if (Value exp = signature["expires"]; exp) {
                int64_t expMinutes = exp.asInt();
                if (expMinutes <= 0)
                    return VerifyResult::InvalidProperties;
                if ((now - start) / 60000 > expMinutes)
                    return VerifyResult::Expired;
            }
        }

        return VerifyResult::Valid;
    }

}
