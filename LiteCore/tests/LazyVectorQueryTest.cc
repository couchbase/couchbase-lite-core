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
#include <cmath>

#ifdef COUCHBASE_ENTERPRISE


using namespace std;
using namespace fleece;

static constexpr size_t kDimension = 5;

static void computeVector(int64_t n, float vec[kDimension]) {
    static constexpr int kPrimes[kDimension] = {2, 3, 5, 7, 11};
    for ( size_t i = 0; i < kDimension; ++i ) {
        float modulo     = ((n % kPrimes[i]) / float(kPrimes[i]));
        float similarity = fabs(modulo - 0.5f) * 2;
        vec[i]           = similarity;
    }
}

class LazyVectorQueryTest : public VectorQueryTest {
  public:
    LazyVectorQueryTest() : LazyVectorQueryTest(0) {}

    LazyVectorQueryTest(int which) : VectorQueryTest(which) {
        addNumberedDocs(1, 400);
        addNonVectorDoc(401);
        createVectorIndex();

        string queryStr = R"(
         ['SELECT', {
            WHERE:    ['VECTOR_MATCH()', 'factorsindex', ['$target'], 5],
            WHAT:     [ ['._id'], ['AS', ['VECTOR_DISTANCE()', 'factorsindex'], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
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
        IndexSpec::VectorOptions options(kDimension);
        options.clustering.type           = IndexSpec::VectorOptions::Flat;
        options.clustering.flat_centroids = 16;
        options.lazy                      = true;
        VectorQueryTest::createVectorIndex("factorsindex", "[  ['.num'] ]", options);

        _lazyIndex = make_retained<LazyIndex>(*store, "factorsindex");
    }

    using UpdaterFn = function_ref<bool(LazyIndexUpdate*, size_t, fleece::Value)>;

    static bool alwaysUpdate(LazyIndexUpdate*, size_t, fleece::Value) { return true; }

    size_t updateVectorIndex(size_t limit, UpdaterFn fn) {
        Log("---- Starting index update...");
        Retained<LazyIndexUpdate> update = _lazyIndex->beginUpdate(limit);
        if ( !update ) {
            Log("...nothing to update");
            return 0;
        }
        Log("---- Updating %zu vectors...", update->count());

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

    void checkQueryReturns(std::vector<slice> expectedIDs) {
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
    Retained<QueryEnumerator> e;
    e = (_query->createEnumerator(&_options));
    REQUIRE(e->getRowCount() == 0);  // index is empty so far

    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 200);
    REQUIRE(updateVectorIndex(999, alwaysUpdate) == 200);

    checkQueryReturns({"rec-291", "rec-171", "rec-039", "rec-081", "rec-249"});

    // Nothing more to update
    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 0);

    addNonVectorDoc(402);  // Add a row that has no 'number' property
    REQUIRE(updateVectorIndex(200, alwaysUpdate) == 0);
}

TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Index Skipping", "[Query][.VectorSearch]") {
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

#endif

// Guard against multiple updater objects, where 2nd one finishes first!!
