//
// PredictiveQueryTest.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
    EightBall(DataFile *db)
    :db(db)
    { }

    DataFile* const db;
    bool allowCalls {true};

    virtual alloc_slice prediction(const Dict* input,
                                   DataFile::Delegate *delegate,
                                   C4Error *outError) noexcept override {
//        Log("8-ball input: %s", input->toJSONString().c_str());
        CHECK(allowCalls);
        CHECK(delegate == db->delegate());
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query unregistered", "[Query][Predict]") {
    addNumberedDocs(1, 10);
    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['PREDICTION()', '8ball', {number: ['.num']}]]}")) };
    ExpectException(error::SQLite, 1, [&]{
        Retained<QueryEnumerator> e(query->createEnumerator());
    });
}


static void testResults(Query *query) {
    Retained<QueryEnumerator> e(query->createEnumerator());
    int docNo = 0;
    while (e->next()) {
        ++docNo;
        auto col = e->columns();
        slice docID = col[0]->asString();
        Log("%.*s : %s", SPLAT(docID), col[1]->toJSONString().c_str());
        if (docNo < 101) {
            REQUIRE(col[1]->asDict());
            CHECK(col[1]->asDict()->get("integer"_sl)->asInt() == 1);
            CHECK(col[1]->asDict()->get("even"_sl)->asBool() == !(docNo % 2));
        } else {
            CHECK(col[1]->type() == kNull);
        }
    }
    CHECK(docNo == 101);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        ExclusiveTransaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }

    Retained<PredictiveModel> model = new EightBall(db.get());
    model->registerAs("8ball");

    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['._id'], ['PREDICTION()', '8ball', {number: ['.num']}]]}")) };
    testResults(query);

    PredictiveModel::unregister("8ball");
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query invalid input", "[Query][Predict]") {
    {
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    Retained<PredictiveModel> model = new EightBall(db.get());
    model->registerAs("8ball");

    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['.value'], ['PREDICTION()', '8ball', ['.value']]]}")) };
    ExpectException(error::SQLite, 1, [&]() {
        Retained<QueryEnumerator> e(query->createEnumerator());
    });

    PredictiveModel::unregister("8ball");
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Create/Delete Predictive Index", "[Query][Predict]") {
    Retained<PredictiveModel> model = new EightBall(db.get());
    model->registerAs("8ball");

    store->createIndex("nums"_sl, json5("[['PREDICTION()', '8ball', {number: ['.num']}, '.square']]"),
                       IndexSpec::kPredictive);
    store->deleteIndex("nums"_sl);

    PredictiveModel::unregister("8ball");
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query indexed", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        ExclusiveTransaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }

    Retained<EightBall> model = new EightBall(db.get());
    model->registerAs("8ball");

    string prediction = "['PREDICTION()', '8ball', {number: ['.num']}, '.square']";

    for (int pass = 1; pass <= 3; ++pass) {
        INFO("During pass #" << pass);
        if (pass > 1) {
            store->createIndex("nums"_sl, json5("["+prediction+"]"),
                               IndexSpec::kPredictive);

            // Now that it's indexed, there should be no more calls to the model:
            model->allowCalls = false;
        }

        // Query numbers in descending order of square-ness:
        Retained<Query> query{ store->compileQuery(json5(
            "{'WHAT': [['.num'], "+prediction+"],"
            " 'ORDER_BY': [['DESC', "+prediction+"],"
                          "['DESC', ['.num']]] }")) };
        string explanation = query->explain();
        Log("Explanation: %s", explanation.c_str());

        if (pass > 1) {
            CHECK(explanation.find("prediction(") == string::npos);
            CHECK(explanation.find("USING INDEX nums") != string::npos);
        }

        vector<int64_t> results;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while (e->next()) {
            if (e->columns()[0]->type() == kNumber)
                results.push_back( e->columns()[0]->asInt() );
        }
        CHECK(results == (vector<int64_t>({ 100, 81, 64, 49, 36, 25, 16, 9, 4, 1, 99, 82, 80, 65, 63, 50, 48, 37, 35, 26, 98, 24, 83, 79, 17, 66, 62, 15, 51,
            47, 97, 10, 38, 84, 78, 34, 8, 67, 61, 27, 96, 23, 52, 46, 85, 77, 5, 18, 39, 68, 95, 60, 33, 14, 3, 86, 53, 76, 28,
            45, 94, 69, 22, 11, 59, 40, 87, 75, 32, 54, 7, 93, 19, 70, 44, 88, 58, 29, 13, 74, 41, 92, 2, 55, 21, 71, 31, 89, 43,
            6, 57, 73, 91, 12, 20, 30, 42, 56, 72, 90 })));
    }
    PredictiveModel::unregister("8ball");
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query compound indexed", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        ExclusiveTransaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }
    
    Retained<EightBall> model = new EightBall(db.get());
    model->registerAs("8ball");
    
    string square = "['PREDICTION()', '8ball', {number: ['.num']}, '.square']";
    string even = "['PREDICTION()', '8ball', {number: ['.num']}, '.even']";
    
    for (int pass = 1; pass <= 3; ++pass) {
        INFO("During pass #" << pass);
        if (pass > 1) {
            string index = "['PREDICTION()', '8ball', {number: ['.num']}, '.square', '.even']";
            store->createIndex("nums"_sl, json5("["+index+"]"),
                               IndexSpec::kPredictive);
            
            // Now that it's indexed, there should be no more calls to the model:
            model->allowCalls = false;
        }
        
        // Query numbers in descending order of square-ness:
        Retained<Query> query{ store->compileQuery(json5(
            "{'WHAT': [['.num'], "+square+"],"
            " 'WHERE': ['AND', ['>=', "+square+", 1], ['>=', "+even+", 1]],"
            " 'ORDER_BY': [['DESC', ['.num']]] }" )) };
        string explanation = query->explain();
        Log("Explanation: %s", explanation.c_str());
        
        if (pass > 1) {
            CHECK(explanation.find("prediction(") == string::npos);
            CHECK(explanation.find("USING INDEX nums") != string::npos);
        }
        
        vector<int64_t> results;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while (e->next()) {
            if (e->columns()[0]->type() == kNumber)
                results.push_back( e->columns()[0]->asInt() );
        }
        CHECK(results == (vector<int64_t>({ 100, 64, 36, 16, 4 })));
    }
    PredictiveModel::unregister("8ball");
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Predictive Query cached only", "[Query][Predict]") {
    addNumberedDocs(1, 100);
    {
        ExclusiveTransaction t(db);
        writeArrayDoc(101, t);      // Add a row that has no 'num' property
        t.commit();
    }
    
    Retained<EightBall> model = new EightBall(db.get());
    model->registerAs("8ball");
    
    string square = "['PREDICTION()', '8ball', {number: ['.num']}, '.square']";
    string even = "['PREDICTION()', '8ball', {number: ['.num']}, '.even']";
    
    for (int pass = 1; pass <= 3; ++pass) {
        INFO("During pass #" << pass);
        if (pass > 1) {
            string index = "['PREDICTION()', '8ball', {number: ['.num']}]";
            store->createIndex("nums"_sl, json5("["+index+"]"),
                               IndexSpec::kPredictive);
            
            // Now that it's indexed, there should be no more calls to the model:
            model->allowCalls = false;
        }
        
        // Query numbers in descending order of square-ness:
        Retained<Query> query{ store->compileQuery(json5(
            "{'WHAT': [['.num'], "+square+"],"
            " 'WHERE': ['AND', ['>=', "+square+", 1], ['>=', "+even+", 1]],"
            " 'ORDER_BY': [['DESC', ['.num']]] }" )) };
        string explanation = query->explain();
        Log("Explanation: %s", explanation.c_str());
        
        if (pass > 1) {
            CHECK(explanation.find("prediction(") == string::npos);
            CHECK(explanation.find("USING INDEX nums") == string::npos);
        }
        
        vector<int64_t> results;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while (e->next()) {
            if (e->columns()[0]->type() == kNumber)
                results.push_back( e->columns()[0]->asInt() );
        }
        CHECK(results == (vector<int64_t>({ 100, 64, 36, 16, 4 })));
    }
    PredictiveModel::unregister("8ball");
}

#endif // COUCHBASE_ENTERPRISE
