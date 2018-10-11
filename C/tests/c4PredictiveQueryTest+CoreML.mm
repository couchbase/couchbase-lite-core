//
// c4PredictiveQueryTest+CoreML.mm
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

#include "c4PredictiveQuery.h"
#include "CoreMLPredictiveModel.hh"
#include "c4QueryTest.hh"
#include "fleece/Fleece.hh"
#include <CoreML/CoreML.h>

using namespace fleece;


#ifdef COUCHBASE_ENTERPRISE


static NSString* asNSString(const string &str) {
    return [NSString stringWithUTF8String: str.c_str()];
}


// Test class that uses a CoreML model
class API_AVAILABLE(macos(10.13)) CoreMLTest : public QueryTest {
public:
    // The default model file comes from Apple's MarsHabitatPricePredictor sample app.
    CoreMLTest()
    :CoreMLTest("mars.json", "mars", "MarsHabitatPricer.mlmodel")
    { }

    CoreMLTest(const string &jsonFilename,
               const char *modelName,
               const string &modelFilename,
               bool required =true)
    :QueryTest(0, jsonFilename)
    {
        NSURL *url = [NSURL fileURLWithPath: asNSString(sFixturesDir + modelFilename)];
        NSError *error;
        if (required || [url checkResourceIsReachableAndReturnError: nullptr]) {
            NSURL* compiled = [MLModel compileModelAtURL: url error: &error];
            INFO("Error" << (error.description.UTF8String ?: "none"));
            REQUIRE(compiled);
            auto model = [MLModel modelWithContentsOfURL: compiled error: &error];
            INFO("Error" << (error.description.UTF8String ?: "none"));
            REQUIRE(model);
            _model.reset(new cbl::CoreMLPredictiveModel(model));
            _model->registerWithName(modelName);
        } else {
            C4Log("*** SKIPPING test, as CoreML model '%s' is not present **", modelFilename.c_str());
        }
    }

    void checkQueryError(const char *queryStr, const char *expectedErrorMessage) {
        compileSelect(json5(queryStr));
        C4QueryOptions options = kC4DefaultQueryOptions;
        C4Error error = {};
        ExpectingExceptions x;
        auto e = c4query_run(query, &options, nullslice, &error);
        CHECK(!e);
        char errbuf[256];
        C4Log("Error is %s", c4error_getDescriptionC(error, errbuf, sizeof(errbuf)))
        CHECK(error.domain == SQLiteDomain);
        CHECK(error.code != 0);
        alloc_slice msg = c4error_getMessage(error);
        CHECK(string(msg) == expectedErrorMessage);
    }

    unique_ptr<cbl::CoreMLPredictiveModel> _model;
};


TEST_CASE_METHOD(CoreMLTest, "CoreML Query", "[Query][Predict][C]") {
    compileSelect(json5("{'WHAT': [['._id'], ['PREDICTION()', 'mars', "
                        "{solarPanels: ['.panels'], greenhouses: ['.greenhouses'], size: ['.acres']}"
                        "]], 'ORDER_BY': [['._id']]}"));
    auto results = runCollecting<double>(nullptr, [=](C4QueryEnumerator *e) {
        Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
        C4Log("result: %.*s", SPLAT(val.toJSON()));
        return round(val.asDict()["price"].asDouble());
    });
    CHECK(results == (vector<double>{1566, 16455, 28924}));
    // Expected results come from running Apple's MarsHabitatPricePredictor sample app.
}


TEST_CASE_METHOD(CoreMLTest, "CoreML Query Error", "[Query][Predict][C]") {
    // Missing 'greenhouses' parameter:
    compileSelect(json5("{'WHAT': [['PREDICTION()', 'mars', "
                        "{solarPanels: ['.panels'], size: ['.acres']}"
                    "]], 'ORDER_BY': [['._id']]}"));
    // 'greenhouses' is of wrong type:
    CHECK(run() == (vector<string>{ "MISSING", "MISSING", "MISSING" }));
    compileSelect(json5("{'WHAT': [['PREDICTION()', 'mars', "
                        "{solarPanels: ['.panels'], greenhouses: 'oops', size: ['.acres']}"
                    "]], 'ORDER_BY': [['._id']]}"));
    CHECK(run() == (vector<string>{ "MISSING", "MISSING", "MISSING" }));
}


class CoreMLSentimentTest : public CoreMLTest {
public:
    CoreMLSentimentTest()
    :CoreMLTest("sentiments.json", "sentiment", "SentimentPolarity.mlmodel")
    { }
};


TEST_CASE_METHOD(CoreMLSentimentTest, "CoreML Sentiment Query", "[Query][Predict][C]") {
    compileSelect(json5("{'WHAT': [['._id'], ['PREDICTION()', 'sentiment', "
                            "{input: ['.text']}"
                        "]], 'ORDER_BY': [['._id']]}"));
    auto results = runCollecting<string>(nullptr, [=](C4QueryEnumerator *e) {
        Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
        C4Log("result: %.*s", SPLAT(val.toJSON()));
        return string(val.asDict()["classLabel"].asString());
    });
    CHECK(results == (vector<string>{"Neg", "Neg", "Pos"}));
}



class CoreMLImageTest : public CoreMLTest {
public:
    CoreMLImageTest()
    :CoreMLTest("", "mobilenet", "imagePrediction/MobileNet.mlmodel", false)
    { }

    void addDocWithImage(const string &baseName) {
        auto image = readFile(sFixturesDir + "imagePrediction/" + baseName + ".jpeg");
        addDocWithAttachments(slice(baseName), {string(image)}, "image/jpeg");
    }
};


// This test is skipped by default because the CoreML model is too large to include in the Git repo.
// You can download it at https://docs-assets.developer.apple.com/coreml/models/MobileNet.mlmodel
// and copy it to C/tests/data/imagePrediction/MobileNet.mlmodel .
TEST_CASE_METHOD(CoreMLImageTest, "CoreML Image Query", "[Query][Predict][C]") {
    if (!_model)
        return;
    {
        TransactionHelper t(db);
        addDocWithImage("cat");
        addDocWithImage("jeep");
        addDocWithImage("pineapple");
        addDocWithImage("waterfall");
    }

    compileSelect(json5("{WHAT: [['._id'],"
                                "['PREDICTION()', 'mobilenet', {image: ['BLOB', '.attached[0]']}]],"
                         "ORDER_BY: [['._id']]}"));
    auto collect = [=](C4QueryEnumerator *e) {
        Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
        alloc_slice json = val.toJSON();
        C4Log("result: %.*s", SPLAT(json));
        slice label = FLValue_AsString( FLDict_Get(FLValue_AsDict(val), "classLabel"_sl) );
        return string(label);
    };
    auto results = runCollecting<string>(nullptr, collect);
    CHECK(results == (vector<string>{
        "Egyptian cat", "jeep, landrover", "pineapple, ananas", "cliff, drop, drop-off"
    }));
}


#endif // COUCHBASE_ENTERPRISE
