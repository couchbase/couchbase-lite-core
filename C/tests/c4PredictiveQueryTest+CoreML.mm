#ifdef COUCHBASE_ENTERPRISE


//
// c4PredictiveQueryTest+CoreML.mm
//
// Copyright © 2018 Couchbase. All rights reserved.
//
//  COUCHBASE LITE ENTERPRISE EDITION
//  Licensed under the Couchbase License Agreement (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//  https://info.couchbase.com/rs/302-GJY-034/images/2017-10-30_License_Agreement.pdf
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "c4PredictiveQuery.h"
#include "CoreMLPredictiveModel.hh"
#include "c4QueryTest.hh"
#include "c4CppUtils.hh"
#include "fleece/Fleece.hh"
#include <CoreML/CoreML.h>
#include <array>

using namespace fleece;
using namespace std;


static NSString* asNSString(const string &str) {
    return [NSString stringWithUTF8String: str.c_str()];
}


// Test class that uses a CoreML model
class CoreMLTest : public C4QueryTest {
public:
    // The default model file comes from Apple's MarsHabitatPricePredictor sample app.
    CoreMLTest()
    :CoreMLTest("mars.json", "mars", "MarsHabitatPricer.mlmodel")
    { }

    CoreMLTest(const string &jsonFilename,
               const char *modelName,
               const string &modelFilename,
               bool required =true)
    :C4QueryTest(0, jsonFilename)
    {
        if (@available(macOS 10.13, iOS 11.0, *)) {
            NSURL *url = [NSURL fileURLWithPath: asNSString(sFixturesDir + modelFilename)];
            NSError *error;
            if (required || [url checkResourceIsReachableAndReturnError: nullptr]) {
                MLModel* model;
                {
                    ExpectingExceptions x;  // CoreML throws & catches exceptions during this
                    NSURL* compiled = [MLModel compileModelAtURL: url error: &error];
                    if (!compiled)
                        INFO("Error" << (error.description.UTF8String ?: "none"));
                    REQUIRE(compiled);
                    model = [MLModel modelWithContentsOfURL: compiled error: &error];
                    if (!model)
                        INFO("Error" << (error.description.UTF8String ?: "none"));
                    REQUIRE(model);
                }
                _model.reset(new cbl::CoreMLPredictiveModel(model));
                _model->registerWithName(modelName);
            } else {
                C4Log("*** SKIPPING test, as CoreML model '%s' is not present **", modelFilename.c_str());
            }
        } else {
            C4Log("*** SKIPPING test, as CoreML is not available on this OS **");
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
        C4Log("Error is %s", c4error_getDescriptionC(error, errbuf, sizeof(errbuf)));
        CHECK(error.domain == SQLiteDomain);
        CHECK(error.code != 0);
        alloc_slice msg = c4error_getMessage(error);
        CHECK(string(msg) == expectedErrorMessage);
    }

    unique_ptr<cbl::PredictiveModel> _model;
};


TEST_CASE_METHOD(CoreMLTest, "CoreML Query", "[Query][Predict][C]") {
    if (!_model)
        return;
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
    if (!_model)
        return;
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
    if (!_model)
        return;
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

    string prediction = "['PREDICTION()', 'mobilenet', {image: ['BLOB', '.attached[0]']}, '.classLabel']";
    for (int pass = 0; pass <= 1; ++pass) {
        if (pass == 1) {
            // Create an index:
            C4Log("-------- Creating index");
            REQUIRE(c4db_createIndex(db, C4STR("mobilenet"),
                                     slice("[" + json5(prediction) + "]"), kC4PredictiveIndex, nullptr, nullptr));
        }
        compileSelect(json5("{WHAT: [['._id']," + prediction + "], ORDER_BY: [['._id']]}"));

        alloc_slice explanation(c4query_explain(query));
        C4Log("%.*s", SPLAT(explanation));
        if (pass > 0) {
            CHECK(explanation.find("prediction("_sl) == nullslice);
            CHECK(explanation.find("SEARCH TABLE kv_default:predict:"_sl) != nullslice);
        }

        auto collect = [=](C4QueryEnumerator *e) {
            Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
            alloc_slice json = val.toJSON();
            C4Log("result: %.*s", SPLAT(json));
            slice label = FLValue_AsString(val);
            return string(label);
        };
        auto results = runCollecting<string>(nullptr, collect);
        CHECK(results == (vector<string>{
            "Egyptian cat", "jeep, landrover", "pineapple, ananas", "cliff, drop, drop-off"
        }));

        C4Log("------- Query keyed on classLabel");
        compileSelect(json5("{WHAT: [['._id']], WHERE: ['=', "+prediction+", 'pineapple, ananas'], ORDER_BY: [['._id']]}"));
        explanation = c4query_explain(query);
        C4Log("%.*s", SPLAT(explanation));
        if (pass > 0) {
            CHECK(explanation.find("prediction("_sl) == nullslice);
            CHECK(explanation.find("SCAN"_sl) == nullslice);
        }
    }
}



// These tests are skipped by default because the CoreML model is too large to include in the Git repo.
// You can download it at https://github.com/iwantooxxoox/Keras-OpenFace/blob/master/model/OpenFace.mlmodel
// and copy it to C/tests/data/faces/OpenFace.mlmodel .
class CoreMLFaceTest : public CoreMLTest {
public:
    CoreMLFaceTest()
    :CoreMLTest("", "face", "faces/OpenFace.mlmodel", false)
    {
        if (_model) {
            TransactionHelper t(db);
            addDocWithImage("adams");
            addDocWithImage("carell");
            addDocWithImage("clapton-1");
            addDocWithImage("clapton-2");
            addDocWithImage("lennon-1");
            addDocWithImage("lennon-2");
        }
    }

    void addDocWithImage(const string &baseName) {
        auto image = readFile(sFixturesDir + "faces/" + baseName + ".png");
        addDocWithAttachments(slice(baseName), {string(image)}, "image/png");
    }

    static constexpr const char* kPrediction =
            "['PREDICTION()', 'face', {data: ['.attached[0]']}, '.output']";
    // Note: Not using the [BLOB] operator -- this tests CoreMLPredictiveModel's ability to
    // load the blob itself.

    void createIndex() {
        C4Log("-------- Creating index");
        REQUIRE(c4db_createIndex(db, C4STR("faces"),
                                 slice("[" + json5(kPrediction) + "]"),
                                 kC4PredictiveIndex, nullptr, nullptr));
    }

    using face = array<double,128>;

    double euclideanDistance(const face &a, const face &b) {
        double dist = 0.0;
        for (int i = 0; i < 128; i++)
            dist += (a[i] - b[i]) * (a[i] - b[i]);
        return sqrt(dist);
    }
};

TEST_CASE_METHOD(CoreMLFaceTest, "CoreML face query", "[Query][Predict][C]") {
    if (!_model)
        return;

    auto queryString = json5("{WHAT: [['._id'], " + string(kPrediction) + "], ORDER_BY: [['._id']]}");

    for (int pass = 0; pass <= 1; ++pass) {
        if (pass == 1)
            createIndex();
        compileSelect(queryString);
        alloc_slice explanation(c4query_explain(query));
        C4Log("%.*s", SPLAT(explanation));
        if (pass > 0) {
            CHECK(explanation.find("prediction("_sl) == nullslice);
            CHECK(explanation.find("SEARCH TABLE kv_default:predict:"_sl) != nullslice);
        }

        auto collect = [=](C4QueryEnumerator *e) {
            Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
            string json = val.toJSONString();
            C4Log("result: %s", json.c_str());
            face a;
            for (int i=0; i<128; i++)
                a[i] = val.asArray()[i].asDouble();
            return a;
        };
        auto results = runCollecting<face>(nullptr, collect);

        for (int i = 0; i < results.size(); ++i) {
            for (int j = 0; j <= i; ++j)
                fprintf(stderr, "  %8.5f", euclideanDistance(results[i], results[j]));
            cerr << "\n";
        }
    }
}


TEST_CASE_METHOD(CoreMLFaceTest, "CoreML face similarity query", "[Query][Predict][C]") {
    if (!_model)
        return;

    string similarity = "['euclidean_distance()', ['$vector'], " + string(kPrediction) + "]";

    // Get the vector of one document:
    compileSelect(json5("{WHAT: [" + string(kPrediction) + "], WHERE: ['=', ['._id'], 'lennon-2']}"));
    C4Error error;
    c4::ref<C4QueryEnumerator> e = c4query_run(query, &kC4DefaultQueryOptions, nullslice, ERROR_INFO(error));
    REQUIRE(e);
    REQUIRE(c4queryenum_next(e, ERROR_INFO(error)));
    Value targetVector = Array::iterator(e->columns)[0];

    auto queryString = json5("{WHAT: [['._id'], " + similarity + "], ORDER_BY: [" + similarity + "]}");
    compileSelect(queryString);

    JSONEncoder enc;
    enc.beginDict();
    enc.writeKey("vector"_sl);
    enc.writeValue(targetVector);
    enc.endDict();
    auto params = enc.finish().asString();
    C4Log("Params = %s", params.c_str());

    auto collect = [=](C4QueryEnumerator *e) {
        Array::iterator i(e->columns);
        string docID = i[0].asString().asString();
        double similarity = i[1].asDouble();
        C4Log("%s\t%f", docID.c_str(), similarity);
        return docID;
    };
    auto results = runCollecting<string>(params.c_str(), collect);
    CHECK(results == (vector<string>{ "lennon-2", "lennon-1", "carell", "adams", "clapton-1", "clapton-2" }));
}


#endif // COUCHBASE_ENTERPRISE
