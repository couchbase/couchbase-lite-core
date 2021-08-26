//
// PropertyEncryptionTests.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "PropertyEncryption.hh"
#include "c4Test.hh"
#include "c4CppUtils.hh"
#include "c4ReplicatorTypes.h"
#include "Base64.hh"
#include "fleece/Mutable.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;


LITECORE_UNUSED
static constexpr slice
     kDocID = "i_have_seekrits"
    ,kDefaultCleartext  = "\"123-45-6789\""
    ,kDefaultCiphertext = "XXXXENCRYPTEDXXXX"
    ,kDefaultCiphertextBase64 = "WFhYWEVOQ1JZUFRFRFhYWFg="
    ,kCustomAlgorithm = "Rot13"
    ,kCustomKeyID = "Schlage"
    ,kDefaultKeyPath = "SSN"
    ,kNestedKeyPath = "nested[2].SSN"
;


#pragma mark - TEST CLASSES:


class PropEncryptionTest {
public:

    MutableDict encryptProperties(Dict doc, C4Error *outError =nullptr) {
        _numCallbacks = 0;
        C4Error error;
        auto result = litecore::repl::EncryptDocumentProperties(kDocID, doc,
                                                                _callback, this,
                                                                &error);
        if (outError)
            *outError = error;
        else if (!result)
            REQUIRE(!error);
        return result;
    }

    MutableDict encryptProperties(slice json, C4Error *outError =nullptr) {
        Doc doc = Doc::fromJSON(json);
        auto result = encryptProperties(doc.asDict(), outError);
        if (!outError)
            CHECK((result != nullptr) == MayContainPropertiesToEncrypt(json));
        return result;
    }


#ifdef COUCHBASE_ENTERPRISE
    alloc_slice encrypt(slice documentID,
                        Dict properties,
                        slice keyPath,
                        slice cleartext,
                        C4StringResult* outAlgorithm,
                        C4StringResult* outKeyID,
                        C4Error* outError)
    {
        ++_numCallbacks;
        CHECK(documentID == kDocID);
        if (!_expectedKeyPath.empty())
            CHECK(keyPath == _expectedKeyPath);

        *outAlgorithm = C4StringResult(_algorithm);
        *outKeyID = C4StringResult(_keyID);

        CHECK(cleartext == _expectedCleartext);
        return alloc_slice(kDefaultCiphertext);
    }

    static C4SliceResult encryptionCallback(void* C4NULLABLE context,
                                            C4String documentID,
                                            FLDict properties,
                                            C4String keyPath,
                                            C4Slice cleartext,
                                            C4StringResult* outAlgorithm,
                                            C4StringResult* outKeyID,
                                            C4Error* outError)
    {
        return C4SliceResult(((PropEncryptionTest*)context)->encrypt(documentID, properties,
                                                                     keyPath, cleartext,
                                                                     outAlgorithm, outKeyID,
                                                                     outError));
    }
#else
    int encryptionCallback;
#endif

    slice _expectedKeyPath = kDefaultKeyPath;
    slice _expectedCleartext = kDefaultCleartext;
    C4ReplicatorPropertyEncryptionCallback _callback = &encryptionCallback;
    slice _algorithm;
    slice _keyID;
    int _numCallbacks = 0;
};


class PropDecryptionTest {
public:

    MutableDict decryptProperties(Dict doc, C4Error *outError =nullptr) {
        _numCallbacks = 0;
        C4Error error;
        auto result = litecore::repl::DecryptDocumentProperties(kDocID, doc,
                                                                _callback, this,
                                                                &error);
        if (outError)
            *outError = error;
        else if (!result)
            REQUIRE(!error);
        return result;
    }

    MutableDict decryptProperties(slice json, C4Error *outError =nullptr) {

        Doc doc = Doc::fromJSON(json);
        auto result = decryptProperties(doc.asDict(), outError);
        if (!outError)
            CHECK((result != nullptr) == MayContainPropertiesToDecrypt(json));
        return result;
    }

#ifdef COUCHBASE_ENTERPRISE
    alloc_slice decrypt(slice documentID,
                        Dict properties,
                        slice keyPath,
                        slice ciphertext,
                        slice algorithm,
                        slice keyID,
                        C4Error* outError)
    {
        ++_numCallbacks;
        CHECK(documentID == kDocID);
        if (_expectedKeyPath)
            CHECK(keyPath == _expectedKeyPath);
        CHECK(algorithm == _expectedAlgorithm);
        CHECK(keyID == _expectedKeyID);

        CHECK(ciphertext == _expectedCiphertext);
        return alloc_slice(kDefaultCleartext);
    }

    static C4SliceResult decryptionCallback(void* C4NULLABLE context,
                                            C4String documentID,
                                            FLDict properties,
                                            C4String keyPath,
                                            C4Slice ciphertext,
                                            C4String algorithm,
                                            C4String keyID,
                                            C4Error* outError)
    {
        return C4SliceResult(((PropDecryptionTest*)context)->decrypt(documentID, properties,
                                                                     keyPath, ciphertext,
                                                                     algorithm, keyID,
                                                                     outError));
    }
#else
    int decryptionCallback;
#endif

    C4ReplicatorPropertyDecryptionCallback _callback = &decryptionCallback;
    slice _expectedKeyPath = kDefaultKeyPath;
    slice _expectedCiphertext = kDefaultCiphertext;
    slice _expectedAlgorithm = "CB_MOBILE_CUSTOM";
    slice _expectedKeyID;
    int _numCallbacks = 0;
};


LITECORE_UNUSED
static constexpr slice
     kDecryptedOneProperty = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"
    ,kEncryptedOneProperty = R"({"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"WFhYWEVOQ1JZUFRFRFhYWFg="}})"
    ,kDecryptedCustomAlg = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"
    ,kEncryptedCustomAlg = R"({"encrypted$SSN":{"alg":"Rot13","ciphertext":"WFhYWEVOQ1JZUFRFRFhYWFg=","kid":"Schlage"}})"
    ,kDecryptedNested = R"({"foo":1234,"nested":[0,1,{"SSN":{"@type":"encryptable","value":"123-45-6789"}},3,4]})"
    ,kEncryptedNested = R"({"foo":1234,"nested":[0,1,{"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"WFhYWEVOQ1JZUFRFRFhYWFg="}},3,4]})"
    ,kDecryptedTwoProps = R"({"SSN1":{"@type":"encryptable","value":"123-45-6789"},"SSN2":{"@type":"encryptable","value":"123-45-6789"}})"
    ,kEncryptedTwoProps = R"({"encrypted$SSN1":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"WFhYWEVOQ1JZUFRFRFhYWFg="},"encrypted$SSN2":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"WFhYWEVOQ1JZUFRFRFhYWFg="}})"
;


#pragma mark - ENCRYPTION TESTS:


TEST_CASE_METHOD(PropEncryptionTest, "No Property Encryption", "[Sync][Encryption]") {
    constexpr slice kTestCases[] = {
        "{}",
        "{foo:1234, bar:false}",
        "{foo:1234, bar:[null, true, 'howdy', {}]}",
        "{SSN:{'@type':'CryptidProperty', value:'123-45-6789'}}",
        "{SSN:{'%type':'encryptable', value:'123-45-6789'}}",
    };
    for (slice testCase : kTestCases) {
        CHECK(encryptProperties(ConvertJSON5(string(testCase))) == nullptr);
        CHECK(_numCallbacks == 0);
    }
}


#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(PropEncryptionTest, "Encrypt One Property", "[Sync][Encryption]") {
    MutableDict props = encryptProperties(kDecryptedOneProperty);
    CHECK(_numCallbacks == 1);
    CHECK(slice(props.toJSONString()) == kEncryptedOneProperty);

    slice cipher = props["encrypted$SSN"].asDict()["ciphertext"].asString();
    CHECK(cipher == kDefaultCiphertextBase64);
    CHECK(base64::decode(cipher) == kDefaultCiphertext);
}


TEST_CASE_METHOD(PropEncryptionTest, "Encrypt Custom Alg and KeyID", "[Sync][Encryption]") {
    _algorithm = kCustomAlgorithm;
    _keyID = kCustomKeyID;
    MutableDict props = encryptProperties(kDecryptedCustomAlg);
    CHECK(_numCallbacks == 1);
    CHECK(props.toJSON() == kEncryptedCustomAlg);
}


TEST_CASE_METHOD(PropEncryptionTest, "Encrypt Nested Property", "[Sync][Encryption]") {
    _expectedKeyPath = kNestedKeyPath;
    MutableDict props = encryptProperties(kDecryptedNested);
    CHECK(_numCallbacks == 1);
    CHECK(props.toJSON() == kEncryptedNested);
}


TEST_CASE_METHOD(PropEncryptionTest, "Encrypt Two Properties", "[Sync][Encryption]") {
    _expectedKeyPath = ""; // there are two
    MutableDict props = encryptProperties(kDecryptedTwoProps);
    CHECK(_numCallbacks == 2);
    CHECK(props.toJSON() == kEncryptedTwoProps);
}

TEST_CASE_METHOD(PropEncryptionTest, "Encryption Fails Without Callback", "[Sync][Encryption]") {
    _callback = nullptr;
    C4Error error;
    ExpectingExceptions x;
    auto result = encryptProperties(kDecryptedOneProperty, &error);
    REQUIRE(!result);
    REQUIRE(error == C4Error{LiteCoreDomain, kC4ErrorCrypto});
}


#else

TEST_CASE_METHOD(PropEncryptionTest, "Don't Encrypt Property In CE", "[Sync][Encryption]") {
    C4Error error;
    Doc doc = Doc::fromJSON(kDecryptedOneProperty);
    auto result = litecore::repl::EncryptDocumentProperties(kDocID, doc,
                                                            &encryptionCallback, this,
                                                            &error);
    REQUIRE(!result);
    REQUIRE(error == C4Error{LiteCoreDomain, kC4ErrorCrypto});
}

#endif


#pragma mark - DECRYPTION TESTS:


TEST_CASE_METHOD(PropDecryptionTest, "No Property Decryption", "[Sync][Encryption]") {
    constexpr slice kTestCases[] = {
        "{}",
        "{foo:1234, bar:false}",
        "{foo:1234, bar:[null, true, 'howdy', {}]}",
        "{encrypted_SSN:{'ciphertext':'nope'}}",
    };
    for (slice testCase : kTestCases) {
        CHECK(decryptProperties(ConvertJSON5(string(testCase))) == nullptr);
        CHECK(_numCallbacks == 0);
    }
}


#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(PropDecryptionTest, "Decrypt One Property", "[Sync][Encryption]") {
    MutableDict props = decryptProperties(kEncryptedOneProperty);
    CHECK(_numCallbacks == 1);
    CHECK(props.toJSON() == kDecryptedOneProperty);
}


TEST_CASE_METHOD(PropDecryptionTest, "Decrypt Custom Alg and KeyID", "[Sync][Encryption]") {
    _expectedAlgorithm = kCustomAlgorithm;
    _expectedKeyID = kCustomKeyID;
    MutableDict props = decryptProperties(kEncryptedCustomAlg);
    CHECK(_numCallbacks == 1);
    CHECK(props.toJSON() == kDecryptedCustomAlg);
}


TEST_CASE_METHOD(PropDecryptionTest, "Decrypt Nested Property", "[Sync][Encryption]") {
    _expectedKeyPath = kNestedKeyPath;
    MutableDict props = decryptProperties(kEncryptedNested);
    CHECK(_numCallbacks == 1);
    CHECK(props.toJSON() == kDecryptedNested);
}


TEST_CASE_METHOD(PropDecryptionTest, "Decrypt Two Properties", "[Sync][Encryption]") {
    _expectedKeyPath = nullslice; // there are two
    MutableDict props = decryptProperties(kEncryptedTwoProps);
    CHECK(_numCallbacks == 2);
    CHECK(props.toJSON() == kDecryptedTwoProps);
}

TEST_CASE_METHOD(PropDecryptionTest, "No Decryption Without Callback", "[Sync][Encryption]") {
    _callback = nullptr;
    C4Error error;
    MutableDict props = decryptProperties(kEncryptedOneProperty, &error);
    CHECK(!props); // i.e. doc should be unchanged
    CHECK(!error);
}


#else

TEST_CASE_METHOD(PropDecryptionTest, "Don't Decrypt Property In CE", "[Sync][Encryption]") {
    Doc doc = Doc::fromJSON(kEncryptedOneProperty);
    C4Error error;
    auto result = litecore::repl::DecryptDocumentProperties(kDocID, doc,
                                                            &decryptionCallback, this,
                                                            &error);
    REQUIRE(!result);
    REQUIRE(!error);
}

#endif
