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
#include "FleeceImpl.hh"

using namespace fleece;
using namespace fleece::impl;
using namespace std;


class DocumentKeysTestFixture : public DataFileTestFixture {
public:
    DocumentKeysTestFixture()
    :DataFileTestFixture(0, &DataFile::Options::defaults)
    { }

    alloc_slice convertJSON(const char *json) {
        Encoder enc;
        enc.setSharedKeys(db->documentKeys());
        JSONConverter jc(enc);
        jc.encodeJSON(slice(json));
        REQUIRE(jc.errorCode() == 0);
        auto out = enc.finish();
        REQUIRE(out.size > 0);
        return out;
    }

    void createDoc(const char *docID, const char *json, Transaction &t) {
        store->set(slice(docID), convertJSON(json), t);
    }

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
        CHECK(db->documentKeys()->byKey() == (vector<slice>{slice("foo"), slice("bar"), slice("zog")}));
        t.abort();
    }

    CHECK(db->documentKeys()->byKey() == (vector<slice>{slice("foo"), slice("bar")}));

    Dict::key foo("foo"_sl);
    Dict::key bar("bar"_sl);
    Dict::key zog("zog"_sl);

    {
        Record r = store->get("doc1"_sl);
        REQUIRE(r.exists());
        Retained<Doc> doc = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict *root = doc->asDict();
        REQUIRE(root);
        const Value *fooVal = root->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 1);
        REQUIRE(root->get(bar) == nullptr);
        REQUIRE(root->get(zog) == nullptr);
    }
    {
        Record r = store->get("doc2"_sl);
        REQUIRE(r.exists());
        Retained<Doc> doc = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict *root = doc->asDict();
        REQUIRE(root);
        const Value *fooVal = root->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 2);
        const Value *barVal = root->get(bar);
        REQUIRE(barVal);
        CHECK(barVal->asInt() == 1);
        REQUIRE(root->get(zog) == nullptr);
    }

    // Now add a doc that uses "zog" as a key:
    {
        Transaction t(db);
        createDoc("doc3", "{\"zog\": 17}", t);
        t.commit();
    }
    CHECK(db->documentKeys()->byKey() == (vector<slice>{slice("foo"), slice("bar"), slice("zog")}));

    // Check that the pre-existing Dict::key for zog works now:
    {
        Record r = store->get("doc3"_sl);
        REQUIRE(r.exists());
        Retained<Doc> doc = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict *root = doc->asDict();
        REQUIRE(root);
        const Value *zogVal = root->get(zog);
        REQUIRE(zogVal);
        CHECK(zogVal->asInt() == 17);
        REQUIRE(root->get(foo) == nullptr);
        REQUIRE(root->get(bar) == nullptr);
    }
}
