//
// SignatureTest.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "PublicKey.hh"
#include "SignedDict.hh"
#include "Base64.hh"
#include "Error.hh"
#include "LiteCoreTest.hh"
#include "fleece/Mutable.hh"
#include <iostream>


using namespace litecore;
using namespace litecore::crypto;
using namespace std;
using namespace fleece;


TEST_CASE("RSA Signatures", "[Signatures]") {
    static constexpr slice kDataToSign = "The only thing we learn from history"
                                         " is that people do not learn from history. --Hegel";
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    alloc_slice signature = key->sign(kDataToSign);
    cout << "Signature is " << signature.size << " bytes: " << base64::encode(signature) << endl;

    // Verify:
    CHECK(key->publicKey()->verifySignature(kDataToSign, signature));

    // Verification fails with wrong public key:
    auto key2 = PrivateKey::generateTemporaryRSA(2048);
    CHECK(!key2->publicKey()->verifySignature(kDataToSign, signature));

    // Verification fails with incorrect digest:
    auto badDigest = SHA256(kDataToSign);
    ((uint8_t*)&badDigest)[10]++;
    CHECK(!key->publicKey()->verifySignature(badDigest, signature));

    // Verification fails with altered signature:
    ((uint8_t&)signature[100])++;
    CHECK(!key->publicKey()->verifySignature(kDataToSign, signature));
}


TEST_CASE("Signed Document", "[Signatures]") {
    bool embedKey = GENERATE(false, true);
    cout << "---- Embed key in signature = " << embedKey << endl;

    // Create a signed doc and convert to JSON:
    alloc_slice publicKeyData;
    string json;
    {
        auto priv = Ed25519SigningKey::generate();
        auto pub  = priv.publicKey();
        publicKeyData = pub.data();

        MutableDict doc = MutableDict::newDict();
        doc["name"] = "Oliver Bolliver Butz";
        doc["age"]  = 6;
        cout << "Document: " << doc.toJSONString() << endl;

        MutableDict sig = makeSignature(doc, priv, 5 /*minutes*/, embedKey);
        REQUIRE(sig);
        string sigJson = sig.toJSONString();
        cout << "Signature, " << sigJson.size() << " bytes: " << sigJson << endl;

        CHECK(verifySignature(doc, sig, &pub) == VerifyResult::Valid);

        doc["(sig)"] = sig;             // <-- add signature to doc, in "(sig)" property
        json = doc.toJSONString();
    }
    cout << "Signed Document: " << json << endl;

    // Now parse the JSON and verify the signature:
    {
        Doc parsedDoc = Doc::fromJSON(json);
        Dict doc = parsedDoc.asDict();
        Dict sig = doc["(sig)"].asDict();
        REQUIRE(sig);

        auto parsedKey = getSignaturePublicKey(sig, "Ed25519");
        if (embedKey) {
            REQUIRE(parsedKey);
            CHECK(parsedKey->data() == publicKeyData);
        } else {
            CHECK(!parsedKey);
            parsedKey = make_unique<Ed25519VerifyingKey>(publicKeyData);
        }

        MutableDict unsignedDoc = doc.mutableCopy();
        unsignedDoc.remove("(sig)");    // <-- detach signature to restore doc to signed form

        if (embedKey)
            CHECK(verifySignature(unsignedDoc, sig) == VerifyResult::Valid);
        else
            CHECK(verifySignature(unsignedDoc, sig) == VerifyResult::MissingKey);

        CHECK(verifySignature(unsignedDoc, sig, parsedKey.get()) == VerifyResult::Valid);
    }
}
