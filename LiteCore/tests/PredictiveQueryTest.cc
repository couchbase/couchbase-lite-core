//
// PredictiveQueryTest.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#include "QueryTest.hh"
#include "PredictiveModel.hh"
#include <math.h>

#ifdef COUCHBASE_ENTERPRISE


using namespace std;
using namespace fleece;
using namespace fleece::impl;



class EightBall : public PredictiveModel {
public:
    bool allowCalls {true};

    virtual alloc_slice prediction(const Dict* input, C4Error *outError) noexcept override {
//        Log("8-ball input: %s", input->toJSONString().c_str());
        CHECK(allowCalls);
        const Value *param = input->get("number"_sl);
        if (!param || param->type() != kNumber) {
            Log("8-ball: No 'number' property; returning MISSING");
            return {};
        }
        double n = param->asDouble();

        Encoder enc;
        enc.beginDictionary();
        enc.writeKey("integer");
        enc.writeDouble(intness(n));
        enc.writeKey("even");
        enc.writeDouble(intness(n / 2.0));
        enc.writeKey("square");
        enc.writeDouble(intness(sqrt(n)));
        enc.endDictionary();
        return enc.finish();
    }
    
    static double intness(double n) {
        return abs(n - floor(n) - 0.5) * 2.0;
    }
};


TEST_CASE_METHOD(QueryTest, "Predictive Query unregistered", "[Query][Predict]") {
    addNumberedDocs(1, 10);
    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['PREDICTION()', '8ball', {number: ['.num']}]]}")) };
    ExpectException(error::SQLite, 1, [&]{
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
    });
}


static void testResults(Query *query) {
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    int docNo = 0;
    while (e->next()) {
        ++docNo;
        auto col = e->columns();
        slice docID = col[0]->asString();
        Log("%.*s : %s", SPLAT(docID), col[1]->toJSONString().c_str());
        if (docNo < 101) {
            REQUIRE(col[1]->asDict());
            CHECK(col[1]->asDict()->get("integer"_sl)->asInt() == 1);
            CHECK(col[1]->asDict()->get("even"_sl)->asInt() == !(docNo % 2));
        } else {
            CHECK(col[1]->type() == kNull);
        }
    }
    CHECK(docNo == 101);
}


TEST_CASE_METHOD(QueryTest, "Predictive Query", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        Transaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }

    Retained<PredictiveModel> model = new EightBall();
    model->registerAs("8ball");

    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['._id'], ['PREDICTION()', '8ball', {number: ['.num']}]]}")) };
    testResults(query);

    PredictiveModel::unregister("8ball");
}


TEST_CASE_METHOD(QueryTest, "Predictive Query invalid input", "[Query][Predict]") {
    {
        Transaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    Retained<PredictiveModel> model = new EightBall();
    model->registerAs("8ball");

    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['.value'], ['PREDICTION()', '8ball', ['.value']]]}")) };
    ExpectException(error::SQLite, 1, [&]() {
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
    });

    PredictiveModel::unregister("8ball");
}


TEST_CASE_METHOD(QueryTest, "Create/Delete Predictive Index", "[Query][Predict]") {
    Retained<PredictiveModel> model = new EightBall();
    model->registerAs("8ball");

    store->createIndex("nums"_sl, json5("[['PREDICTION()', '8ball', {number: ['.num']}, '.square']]"),
                       KeyStore::kPredictiveIndex);
    store->deleteIndex("nums"_sl);

    PredictiveModel::unregister("8ball");
}


TEST_CASE_METHOD(QueryTest, "Predictive Query indexed", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        Transaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }

    Retained<EightBall> model = new EightBall();
    model->registerAs("8ball");

    store->createIndex("nums"_sl, json5("[['PREDICTION()', '8ball', {number: ['.num']}, '.square']]"),
                       KeyStore::kPredictiveIndex);

    // Now that it's indexed, there should be no more calls to the model:
    model->allowCalls = false;

    // Query numbers in descending order of square-ness:
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['.num'], ['PREDICTION()', '8ball', {number: ['.num']}, '.square']],"
        " 'ORDER_BY': [['DESC', ['PREDICTION()', '8ball', {number: ['.num']}, '.square']],"
                      "['DESC', ['.num']]] }")) };
    Log("Explanation: %s", query->explain().c_str());
    vector<int64_t> results;
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    while (e->next())
        results.push_back( e->columns()[0]->asInt() );
    CHECK(results == (vector<int64_t>({ 100, 81, 64, 49, 36, 25, 16, 9, 4, 1, 99, 82, 80, 65, 63, 50, 48, 37, 35, 26, 98, 24, 83, 79, 17, 66, 62, 15, 51,
        47, 97, 10, 38, 84, 78, 34, 8, 67, 61, 27, 96, 23, 52, 46, 85, 77, 5, 18, 39, 68, 95, 60, 33, 14, 3, 86, 53, 76, 28,
        45, 94, 69, 22, 11, 59, 40, 87, 75, 32, 54, 7, 93, 19, 70, 44, 88, 58, 29, 13, 74, 41, 92, 2, 55, 21, 71, 31, 89, 43,
        6, 57, 73, 91, 12, 20, 30, 42, 56, 72, 90, 0 })));

    PredictiveModel::unregister("8ball");
}

#endif // COUCHBASE_ENTERPRISE
