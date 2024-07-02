//
// LazyVectorQueryTest.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
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

#include "VectorQueryTest.hh"
#include "LazyIndex.hh"
#include "fleece/Fleece.hh"
#include "fleece/function_ref.hh"
#include "c4Collection.h"

#include <cmath>

#ifdef COUCHBASE_ENTERPRISE


using namespace std;
using namespace fleece;

static constexpr size_t kDimension = 5;

static void computeVector(int64_t n, float vec[kDimension]) {
    static constexpr int kPrimes[kDimension] = {2, 3, 5, 7, 11};
    for ( size_t i = 0; i < kDimension; ++i ) {
        float modulo     = (static_cast<float>(n % kPrimes[i]) / float(kPrimes[i]));
        float similarity = fabs(modulo - 0.5f) * 2;
        vec[i]           = similarity;
    }
}

class LazyVectorQueryTest : public VectorQueryTest {
  public:
    LazyVectorQueryTest() : LazyVectorQueryTest(0) {}

    LazyVectorQueryTest(int which) : VectorQueryTest(which) {
        // Create the $target query param:
        float           targetVector[5] = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
        fleece::Encoder enc;
        enc.beginDict();
        enc.writeKey("target");
        enc.writeData(slice(targetVector, 5 * sizeof(float)));
        enc.endDict();
        _options = Query::Options(enc.finish());
    }

    /// Initialize the test with some docs and a standard vector index
    void initWithIndex() {
        addNumberedDocs(1, 400);
        addNonVectorDoc(401);
        createVectorIndex();

        string queryStr = R"(
         ['SELECT', {
            WHERE:    ['VECTOR_MATCH()', 'factorsindex', ['$target']],
            WHAT:     [ ['._id'], ['AS', ['VECTOR_DISTANCE()', 'factorsindex'], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
            LIMIT: 5
         }] )";
        _query          = store->compileQuery(json5(queryStr), QueryLanguage::kJSON);
        REQUIRE(_query != nullptr);

        // Create the $target query param:
        float           targetVector[5] = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
        fleece::Encoder enc;
        enc.beginDict();
        enc.writeKey("target");
        enc.writeData(slice(targetVector, 5 * sizeof(float)));
        enc.endDict();
        _options = Query::Options(enc.finish());
    }

    void addNonVectorDoc(int n) {
        ExclusiveTransaction t(db);
        writeArrayDoc(n, t);  // Add a row that has no 'number' property
        t.commit();
    }

    void createVectorIndex() {
        IndexSpec::VectorOptions options(kDimension, vectorsearch::FlatClustering{16}, IndexSpec::DefaultEncoding);
        options.lazyEmbedding = true;
        VectorQueryTest::createVectorIndex("factorsindex", "[  ['.num'] ]", options);

        _lazyIndex = make_retained<LazyIndex>(*store, "factorsindex");
    }

    using UpdaterFn = function_ref<bool(LazyIndexUpdate*, size_t, fleece::Value)>;

    static bool alwaysUpdate(LazyIndexUpdate*, size_t, fleece::Value) { return true; }

    /// Get the LazyIndex with the given name. Will return null if the index does not exist.
    [[nodiscard]] Retained<LazyIndex> getLazyIndex(std::string_view name) const noexcept {
        try {
            return make_retained<LazyIndex>(*store, name);
        } catch ( [[maybe_unused]] std::exception& e ) { return nullptr; }
    }

    [[nodiscard]] size_t updateVectorIndex(size_t limit, UpdaterFn fn) const {
        Log("---- Starting index update...");
        Retained<LazyIndexUpdate> update = _lazyIndex->beginUpdate(limit);
        if ( !update ) {
            Log("...nothing to update");
            return 0;
        }
        Log("---- Updating %zu vectors...", update->count());
        CHECK(update->dimensions() == kDimension);

        size_t count = update->count();
        CHECK(count > 0);
        for ( size_t i = 0; i < count; ++i ) {
            fleece::Value val(update->valueAt(i));
            REQUIRE(val.type() == kFLNumber);
            if ( fn(update, i, val) ) {
                int64_t n = val.asInt();
                float   vec[kDimension];
                computeVector(n, vec);
                update->setVectorAt(i, vec, kDimension);
            }
        }

        Log("---- Finishing index update...");
        ExclusiveTransaction txn(db);
        update->finish(txn);
        txn.commit();
        Log("---- End of index update");
        return count;
    }

    void checkQueryReturns(std::vector<slice> expectedIDs) const {
        auto e = _query->createEnumerator(&_options);
        REQUIRE(e->getRowCount() == expectedIDs.size());
        for ( size_t i = 0; i < expectedIDs.size(); ++i ) {
            INFO("i=" << i);
            REQUIRE(e->next());
            slice id       = e->columns()[0]->asString();
            float distance = e->columns()[1]->asFloat();
            Log("%.*s: %.3f", FMTSLICE(id), distance);
            CHECK(id == expectedIDs[i]);
            // CHECK(fabs(distance - expectedDistances[i]) < 0.01);
        }
        CHECK(!e->next());
        Log("done");
    }

    Retained<LazyIndex> _lazyIndex;
    Retained<Query>     _query;
    Query::Options      _options;
};

TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Index", "[Query][.VectorSearch]") {
    initWithIndex();
    Retained<QueryEnumerator> e;
    expectedWarningsLogged = 1;  //DB WARNING SQLite warning: vectorsearch: Untrained index; queries may be slow.
    e                      = (_query->createEnumerator(&_options));
    REQUIRE(e->getRowCount() == 0);  // index is empty so far

    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 200);
    REQUIRE(updateVectorIndex(999, alwaysUpdate) == 200);

    checkQueryReturns({"rec-291", "rec-171", "rec-039", "rec-081", "rec-249"});

    // Nothing more to update
    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 0);

    addNonVectorDoc(402);  // Add a row that has no 'number' property
    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 0);
}

// 21
TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Index Skipping", "[Query][.VectorSearch]") {
    initWithIndex();
    unsigned nSkipped = 0;
    size_t   n        = updateVectorIndex(999, [&](LazyIndexUpdate* update, size_t i, fleece::Value val) {
        if ( i % 10 == 0 ) {
            update->skipVectorAt(i);  // Skip the docs whose ID ends in 1
            ++nSkipped;
            return false;
        } else {
            return true;
        }
    });
    CHECK(n == 400);

    // rec-291, rec-171 and rec-081 are missing because unindexed
    expectedWarningsLogged = 1;  //DB WARNING SQLite warning: vectorsearch: Untrained index; queries may be slow.
    checkQueryReturns({"rec-039", "rec-249", "rec-345", "rec-159", "rec-369"});

    // Update the index again; only the skipped docs will appear this time.
    size_t nIndexed = 0;
    do {
        n = updateVectorIndex(50, [](LazyIndexUpdate* update, size_t i, fleece::Value val) {
            return true;  // index them
        });
        nIndexed += n;
    } while ( n > 0 );
    CHECK(nIndexed == nSkipped);

    // Now everything is indexed:
    CHECK(updateVectorIndex(200, alwaysUpdate) == 0);
    checkQueryReturns({"rec-291", "rec-171", "rec-039", "rec-081", "rec-249"});
}

TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Update Wrong Dimensions", "[.VectorSearch]") {
    initWithIndex();
    Retained<LazyIndexUpdate> update = _lazyIndex->beginUpdate(1);
    REQUIRE(update);
    CHECK(update->count() == 1);
    CHECK(update->dimensions() == kDimension);

    fleece::Value val(update->valueAt(0));
    REQUIRE(val.type() == kFLNumber);
    float vec[kDimension];
    computeVector(0, vec);

    ExpectingExceptions x;
    Log("---- Calling setVectorAt with wrong dimension...");
    CHECK_THROWS_AS(update->setVectorAt(0, vec, kDimension - 1), error);
}

// 8
TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Modify Docs not Auto-Updated", "[Query][.VectorSearch]") {
    initWithIndex();
    expectedWarningsLogged = 1;  //DB WARNING SQLite warning: vectorsearch: Untrained index; queries may be slow.
    checkQueryReturns({});

    {
        ExclusiveTransaction t(db);

        auto doc1 = getNumberedDoc(1);
        auto doc3 = getNumberedDoc(3);
        writeNumberedDoc(301, doc1.body(), t);
        writeNumberedDoc(1, doc3.body(), t);
    }
    ++expectedWarningsLogged;  //DB WARNING SQLite warning: vectorsearch: Untrained index; queries may be slow.
    checkQueryReturns({});
}

// 9, 10
TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Delete Docs Auto-Updated", "[Query][.VectorSearch]") {
    initWithIndex();
    REQUIRE(updateVectorIndex(1, alwaysUpdate) == 1);

    SECTION("Delete") { deleteDoc(numberedDocID(1), false); }
    SECTION("Purge") { deleteDoc(numberedDocID(1), true); }

    CHECK(updateVectorIndex(1, alwaysUpdate) == 1);
}

#endif
