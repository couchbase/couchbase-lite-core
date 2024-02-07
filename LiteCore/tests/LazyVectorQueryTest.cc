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
    LazyVectorQueryTest(int which) : VectorQueryTest(which) {}

    void makeDocs() {
        addNumberedDocs(1, 400);
        {
            ExclusiveTransaction t(db);
            writeArrayDoc(401, t);  // Add a row that has no 'number' property
            t.commit();
        }
    }

    void createVectorIndex() {
        IndexSpec::VectorOptions options(kDimension);
        options.clustering.type           = IndexSpec::VectorOptions::Flat;
        options.clustering.flat_centroids = 16;
        options.lazy                      = true;
        VectorQueryTest::createVectorIndex("factorsindex", "[  ['.num'] ]", options);

        _lazyIndex = make_retained<LazyIndex>(*store, "factorsindex");
    }

    bool updateVectorIndex(size_t limit, size_t expectedCount) {
        Log("Starting index update...");
        auto update = _lazyIndex->beginUpdate(limit);
        if ( !update ) {
            Log("...nothing to update");
            CHECK(0 == expectedCount);
            return false;
        }
        Log("Updating %zu vectors...", update->count());

        CHECK(update->count() == expectedCount);
        for ( size_t i = 0; i < update->count(); ++i ) {
            fleece::Value val(update->valueAt(i));
            if ( val.type() != kFLNull ) {
                REQUIRE(val.type() == kFLNumber);
                int64_t n = val.asInt();
                float   vec[kDimension];
                computeVector(n, vec);
                update->setVectorAt(i, vec, kDimension);
            }
        }
        Log("Finishing index update...");
        ExclusiveTransaction txn(db);
        update->finish(txn);
        txn.commit();
        return true;
    }

    void testResults(Query* query) {
        Retained<QueryEnumerator> e(query->createEnumerator());
        int                       docNo = 0;
        while ( e->next() ) {
            ++docNo;
            auto  col   = e->columns();
            slice docID = col[0]->asString();
            Log("%.*s : %s", SPLAT(docID), col[1]->toJSONString().c_str());
        }
        CHECK(docNo == 401);
    }

    Retained<LazyIndex> _lazyIndex;
};

N_WAY_TEST_CASE_METHOD(LazyVectorQueryTest, "Lazy Vector Query", "[Query][Predict][.VectorSearch]") {
    makeDocs();
    createVectorIndex();
    string          queryStr = R"(
         ['SELECT', {
            WHERE:    ['VECTOR_MATCH()', 'factorsindex', ['$target'], 5],
            WHAT:     [ ['._id'], ['AS', ['VECTOR_DISTANCE()', 'factorsindex'], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
         }] )";
    Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};
    REQUIRE(query != nullptr);

    // Create the $target query param:
    float           targetVector[5] = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    fleece::Encoder enc;
    enc.beginDict();
    enc.writeKey("target");
    enc.writeData(slice(targetVector, 5 * sizeof(float)));
    enc.endDict();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e;
    e = (query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 0);  // index is empty so far
    expectedWarningsLogged++;      // This will warn "vectorsearch: Querying without an index"

    REQUIRE(updateVectorIndex(200, 200));
    REQUIRE(updateVectorIndex(999, 200));

    e = (query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 5);  // the call to VECTOR_MATCH requested only 5 results

    for ( size_t i = 0; i < 5; ++i ) {
        INFO("i=" << i);
        REQUIRE(e->next());
        slice id       = e->columns()[0]->asString();
        float distance = e->columns()[1]->asFloat();
        Log("%.*s: %.3f", FMTSLICE(id), distance);
        //        CHECK(id == expectedIDs[i]);
        //        CHECK(fabs(distance - expectedDistances[i]) < 0.01);
    }
    CHECK(!e->next());
    Log("done");

    REQUIRE(!updateVectorIndex(200, 0));
}

#endif

// Guard against multiple updater objects, where 2nd one finishes first!!
