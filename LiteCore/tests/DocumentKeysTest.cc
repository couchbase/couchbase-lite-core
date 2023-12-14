//
// DocumentKeysTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LiteCoreTest.hh"
#include "FleeceImpl.hh"

using namespace fleece;
using namespace fleece::impl;
using namespace std;

class DocumentKeysTestFixture : public DataFileTestFixture {
  public:
    DocumentKeysTestFixture() : DataFileTestFixture(0, &DataFile::Options::defaults) {}

    alloc_slice convertJSON(const char* json) {
        Encoder enc;
        enc.setSharedKeys(db->documentKeys());
        JSONConverter jc(enc);
        jc.encodeJSON(slice(json));
        REQUIRE(jc.errorCode() == 0);
        auto out = enc.finish();
        REQUIRE(out.size > 0);
        return out;
    }

    void createDoc(const char* docID, const char* json, ExclusiveTransaction& t) {
        DataFileTestFixture::createDoc(slice(docID), convertJSON(json), t);
    }
};

TEST_CASE_METHOD(DocumentKeysTestFixture, "Create docs", "[SharedKeys]") {
    {
        ExclusiveTransaction t(db);
        createDoc("doc1", "{\"foo\": 1}", t);
        createDoc("doc2", R"({"foo": 2, "bar": 1})", t);
        t.commit();
    }

    // Add "zog" as a key, but abort the transaction so it doesn't take effect:
    {
        ExclusiveTransaction t(db);
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
        Retained<Doc> doc  = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict*   root = doc->asDict();
        REQUIRE(root);
        const Value* fooVal = root->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 1);
        REQUIRE(root->get(bar) == nullptr);
        REQUIRE(root->get(zog) == nullptr);
    }
    {
        Record r = store->get("doc2"_sl);
        REQUIRE(r.exists());
        Retained<Doc> doc  = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict*   root = doc->asDict();
        REQUIRE(root);
        const Value* fooVal = root->get(foo);
        REQUIRE(fooVal);
        CHECK(fooVal->asInt() == 2);
        const Value* barVal = root->get(bar);
        REQUIRE(barVal);
        CHECK(barVal->asInt() == 1);
        REQUIRE(root->get(zog) == nullptr);
    }

    // Now add a doc that uses "zog" as a key:
    {
        ExclusiveTransaction t(db);
        createDoc("doc3", "{\"zog\": 17}", t);
        t.commit();
    }
    CHECK(db->documentKeys()->byKey() == (vector<slice>{slice("foo"), slice("bar"), slice("zog")}));

    // Check that the pre-existing Dict::key for zog works now:
    {
        Record r = store->get("doc3"_sl);
        REQUIRE(r.exists());
        Retained<Doc> doc  = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
        const Dict*   root = doc->asDict();
        REQUIRE(root);
        const Value* zogVal = root->get(zog);
        REQUIRE(zogVal);
        CHECK(zogVal->asInt() == 17);
        REQUIRE(root->get(foo) == nullptr);
        REQUIRE(root->get(bar) == nullptr);
    }
}

TEST_CASE_METHOD(DocumentKeysTestFixture, "Caching of SharedKeys", "[SharedKeys]") {
    {
        ExclusiveTransaction t(db);
        createDoc("doc1", R"({"foo": 1})", t);
        t.commit();
    }

    SECTION("PersistentSharedKeys") {
        Dict::key     foo("foo"_sl);
        Dict::key     bar("bar"_sl);
        Dict::key     zog("zog"_sl);
        Retained<Doc> doc1;
        Retained<Doc> doc2;
        {
            ExclusiveTransaction t(db);
            createDoc("doc2", R"({"bar": "bar"})", t);
            CHECK(db->documentKeys()->byKey() == (vector<slice>{"foo"_sl, "bar"_sl}));

            Record r = store->get("doc1"_sl);
            REQUIRE(r.exists());
            doc1 = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
            REQUIRE(doc1);

            r = store->get("doc2"_sl);
            REQUIRE(r.exists());
            doc2 = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
            REQUIRE(doc2);

            const Value* val1 = doc1->asDict()->get(foo);
            const Value* val2 = doc2->asDict()->get(bar);
            REQUIRE((val1 && val1->asInt() == 1));
            REQUIRE((val2 && val2->asString() == "bar"_sl));

            t.abort();
        }
        CHECK(db->documentKeys()->byKey() == (vector<slice>{"foo"_sl}));

        {
            ExclusiveTransaction t(db);
            createDoc("doc2", R"({"zog": 4})", t);
            CHECK(db->documentKeys()->byKey() == (vector<slice>{"foo"_sl, "zog"_sl}));

            Record r = store->get("doc2"_sl);
            REQUIRE(r.exists());
            doc2 = new Doc(r.body(), Doc::kTrusted, db->documentKeys());
            REQUIRE(doc2);
            const Value* val2 = doc2->asDict()->get(zog);
            REQUIRE((val2 && val2->asInt() == 4));

            t.commit();
        }

        // Following keys are generated in transactions, and hence not sharable.
        CHECK(!foo.isShared());
        CHECK(!bar.isShared());
        CHECK(!zog.isShared());

        const Value* val1 = doc1->asDict()->get(foo);
        REQUIRE((val1 && val1->asInt() == 1));
        CHECK(foo.isShared());  // foo is now sharable because it is reassigned out of transactions.

        const Value* barVal = doc2->asDict()->get(bar);
        const Value* zogVal = doc2->asDict()->get(zog);
        // doc2 == {"zog": 4}
        CHECK(!barVal);  // doc2 does not have key "bar"
        CHECK(zog.isShared());
        CHECK((zogVal && zogVal->asInt() == 4));
    }

    SECTION("Non PersistentSharedKeys") {
        Retained<fleece::impl::SharedKeys> sharedKeys = new fleece::impl::SharedKeys{db->documentKeys()->stateData()};
        Record                             r          = store->get("doc1"_sl);
        REQUIRE(r.exists());
        // Re-encode "doc1" with sharedKeys
        Retained<Doc> doc1 = new Doc(r.body(), Doc::kTrusted, sharedKeys);
        REQUIRE(doc1);
        {
            Dict::key    foo("foo"_sl);
            const Value* val1 = doc1->asDict()->get(foo);
            REQUIRE((val1 && val1->asInt() == 1));
            // By default, SharedKeys is cacheable.
            CHECK(foo.isShared());
        }

        {
            // sharedKeys would become invalid if document is modified afterwards.
            // To avoid it, we can disable the caching.
            // c.f. DBAccess::updateTempSharedKeys
            sharedKeys->disableCaching();
            Dict::key    foo("foo"_sl);
            const Value* val1 = doc1->asDict()->get(foo);
            REQUIRE((val1 && val1->asInt() == 1));
            // Not cached.
            CHECK(!foo.isShared());
        }
    }
}
