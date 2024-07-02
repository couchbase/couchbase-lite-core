//
// PredictiveVectorQueryTest.cc
//
// Copyright © 2023 Couchbase. All rights reserved.
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
#include "PredictiveModel.hh"
#include <cmath>

#ifdef COUCHBASE_ENTERPRISE


using namespace std;
using namespace fleece;
using namespace fleece::impl;

static constexpr int kPrimes[] = {2, 3, 5, 7, 11};

/// Trivial model that takes the "number" property of the input and produces output with a "factors"
/// vector whose elements are the number's closeness to a multiple of the primes listed above.
class FactorsModel : public PredictiveModel {
  public:
    explicit FactorsModel(DataFile* db) : db(db) {}

    DataFile* const db;
    bool            allowCalls{true};

    alloc_slice prediction(const Dict* input, DataFile::Delegate* delegate, C4Error* outError) noexcept override {
        Log("FactorsModel input: %s", input->toJSONString().c_str());
        CHECK(allowCalls);
        CHECK(delegate == db->delegate());
        const Value* param = input->get("number"_sl);
        if ( !param || param->type() != kNumber ) {
            Log("ModuloModel: No 'number' property; returning MISSING");
            return {};
        }
        int64_t n = param->asInt();

        Encoder enc;
        enc.beginDictionary();
        enc.writeKey("vec");
        enc.beginArray();
        for ( int prime : kPrimes ) {
            float modulo     = ((n % prime) / float(prime));
            float similarity = fabs(modulo - 0.5f) * 2;
            enc.writeFloat(similarity);
        }
        enc.endArray();
        enc.endDictionary();
        return enc.finish();
    }
};

class PredictiveVectorQueryTest : public VectorQueryTest {
  public:
    PredictiveVectorQueryTest(int which) : VectorQueryTest(which) {
        make_retained<FactorsModel>(db.get())->registerAs("factors");
    }

    ~PredictiveVectorQueryTest() { PredictiveModel::unregister("factors"); }

    void makeDocs() {
        addNumberedDocs(1, 400);
        {
            ExclusiveTransaction t(db);
            writeArrayDoc(401, t);  // Add a row that has no 'number' property
            t.commit();
        }
    }

    void createVectorIndex(QueryLanguage lang) {
        IndexSpec::VectorOptions options(5, vectorsearch::FlatClustering{16}, IndexSpec::DefaultEncoding);
        if ( lang == QueryLanguage::kJSON ) {
            VectorQueryTest::createVectorIndex(
                    "factorsindex", "[ ['PREDICTION()', 'factors', {number: ['.num']}, '.vec'] ]", options, lang);
        } else if ( lang == QueryLanguage::kN1QL ) {
            VectorQueryTest::createVectorIndex("factorsindex", "PREDICTION(factors, {\"number\": num}, \".vec\")",
                                               options, lang);
        } else {
            REQUIRE(false);
        }
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
};

N_WAY_TEST_CASE_METHOD(PredictiveVectorQueryTest, "Predictive Query of Factors", "[Query][Predict][.VectorSearch]") {
    makeDocs();
    bool n1ql = GENERATE(false, true);
    Log("--- with %s Query", n1ql ? "N1QL" : "JSON");
    Retained<Query> query;
    if ( n1ql ) {
        query = store->compileQuery(R"(SELECT META().id, PREDICTION(factors, {"numbers" : num}, vec) FROM )"s
                                            + collectionName,
                                    QueryLanguage::kN1QL);
    } else {
        query = store->compileQuery(
                json5("{'WHAT': [['._id'], ['PREDICTION()', 'factors', {number: ['.num']}, '.vec']]}"));
    }
    testResults(query);
}

N_WAY_TEST_CASE_METHOD(PredictiveVectorQueryTest, "Vector Index Of Prediction", "[Query][Predict][.VectorSearch]") {
    makeDocs();
    if ( collectionName == "_default" ) {
        createVectorIndex(QueryLanguage::kJSON);
    } else {
        createVectorIndex(QueryLanguage::kN1QL);
    }
    string          queryStr = R"(
         ['SELECT', {
            WHAT:     [ ['._id'], ['AS', ['VECTOR_DISTANCE()', ['PREDICTION()', 'factors', {number: ['.num']}, '.vec'], ['$target']], 'distance'] ],
            ORDER_BY: [ ['.distance'] ],
            LIMIT:    5
         }] )";
    Retained<Query> query{store->compileQuery(json5(queryStr), QueryLanguage::kJSON)};
    REQUIRE(query != nullptr);

    // Create the $target query param:
    float   targetVector[5] = {0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    Encoder enc;
    enc.beginDictionary();
    enc.writeKey("target");
    enc.writeData(slice(targetVector, 5 * sizeof(float)));
    enc.endDictionary();
    Query::Options options(enc.finish());

    // Run the query:
    Retained<QueryEnumerator> e(query->createEnumerator(&options));
    REQUIRE(e->getRowCount() == 5);  // the call to VECTOR_MATCH requested only 5 results

    //    static constexpr slice expectedIDs[10] = {"rec-002", "rec-003", "rec-001", "rec-004", "rec-005",
    //        "rec-006", "rec-007", "rec-008", "rec-009", "rec-010"};
    //
    //    static constexpr float expectedDistances[10] = {0.03f, 0.06f, 0.1f,  0.19f, 0.42f,
    //        0.75f, 1.18f, 1.71f, 2.34f, 3.07f};

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
}

#endif
