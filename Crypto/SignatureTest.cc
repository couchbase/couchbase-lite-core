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
#include "Base64.hh"
#include "Error.hh"
#include "LiteCoreTest.hh"
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
