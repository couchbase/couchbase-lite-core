//
// DocumentKeysTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "LiteCoreTest.hh"
#include "Fleece.hh"

using namespace fleece;


class DocumentKeysTestFixture : public DataFileTestFixture {
public:
    static const DataFile::Options kOptions;

    DocumentKeysTestFixture()
    :DataFileTestFixture(0, &kOptions)
    { }

    alloc_slice convertJSON(const char *json) {
        Encoder enc;
        enc.setSharedKeys(db->documentKeys());
        JSONConverter jc(enc);
        jc.encodeJSON(slice(json));
        REQUIRE(jc.errorCode() == 0);
        auto out = enc.extractOutput();
        REQUIRE(out.size > 0);
        return out;
    }

    void createDoc(const char *docID, const char *json, Transaction &t) {
        store->set(slice(docID), convertJSON(json), t);
    }

};


const DataFile::Options DocumentKeysTestFixture::kOptions = {
    {true},
    true,
    true,
    true,       // useDocumentKeys
};


TEST_CASE_METHOD(DocumentKeysTestFixture, "Create docs", "[SharedKeys]") {
    {
        Transaction t(db);
        createDoc("doc1", "{\"foo\": 1}", t);
        createDoc("doc2", "{\"foo\": 2, \"bar\": 1}", t);
        t.commit();
    }

    // Add "zog" as a key, but abort the transaction so it doesn't take effect:
    {
        Transaction t(db);
        createDoc("doc3", "{\"zog\": 17}", t);
        CHECK(db->documentKeys()->byKey() == (vector<alloc_slice>{alloc_slice("foo"), alloc_slice("bar"), alloc_slice("zog")}));
        t.abort();
    }

    CHECK(db->documentKeys()->byKey() == (vector<alloc_slice>{alloc_slice("foo"), alloc_slice("bar")}));

    Dict::key foo("foo"_sl, db->documentKeys());
    Dict::key bar("bar"_sl, db->documentKeys());
    Dict::key zog("zog"_sl, db->documentKeys());

    {
        Record r = store->get("doc1"_sl);
        REQUIRE(r.exists());
        const Dict *doc = Value::fromData(r.body())->asDict();
        REQUIRE(doc);
        const Value *fooVal = doc->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 1);
        REQUIRE(doc->get(bar) == nullptr);
        REQUIRE(doc->get(zog) == nullptr);
    }
    {
        Record r = store->get("doc2"_sl);
        REQUIRE(r.exists());
        const Dict *doc = Value::fromData(r.body())->asDict();
        REQUIRE(doc);
        const Value *fooVal = doc->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 2);
        const Value *barVal = doc->get(bar);
        REQUIRE(barVal);
        CHECK(barVal->asInt() == 1);
        REQUIRE(doc->get(zog) == nullptr);
    }

    // Now add a doc that uses "zog" as a key:
    {
        Transaction t(db);
        createDoc("doc3", "{\"zog\": 17}", t);
        t.commit();
    }
    CHECK(db->documentKeys()->byKey() == (vector<alloc_slice>{alloc_slice("foo"), alloc_slice("bar"), alloc_slice("zog")}));

    // Check that the pre-existing Dict::key for zog works now:
    {
        Record r = store->get("doc3"_sl);
        REQUIRE(r.exists());
        const Dict *doc = Value::fromData(r.body())->asDict();
        REQUIRE(doc);
        const Value *zogVal = doc->get(zog);
        REQUIRE(zogVal);
        CHECK(zogVal->asInt() == 17);
        REQUIRE(doc->get(foo) == nullptr);
        REQUIRE(doc->get(bar) == nullptr);
    }
}
