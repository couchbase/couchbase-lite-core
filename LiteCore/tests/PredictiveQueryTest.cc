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
    virtual alloc_slice predict(const Value* input) noexcept override {
        string json = input->toJSONString();
        Log("8-ball input: %s", json.c_str());
        Encoder enc;
        enc.beginDictionary();
        enc.writeKey("JSON");
        enc.writeString(json);
        enc.writeKey("number");
        if (input->type() == kNumber) {
            enc.writeDouble(1.0);
            double n = input->asDouble();
            enc.writeKey("integer");
            enc.writeDouble(intness(n));
            enc.writeKey("even");
            enc.writeDouble(intness(n / 2.0));
            enc.writeKey("square");
            enc.writeDouble(intness(sqrt(n)));
        } else {
            enc.writeDouble(0.0);
        }
        enc.endDictionary();
        return enc.finish();
    }
    
    static double intness(double n) {
        return abs(n - floor(n) - 0.5) * 2.0;
    }
};


TEST_CASE_METHOD(QueryTest, "Predictive Query Errors", "[Query][Predict]") {
    addNumberedDocs(1, 10);
    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['PREDICTION()', '8ball', ['.value']]]}")) };
    ExpectException(error::SQLite, 1, [&]{
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
    });
}


TEST_CASE_METHOD(QueryTest, "Predictive Query", "[Query][Predict]") {
    Retained<PredictiveModel> model = new EightBall();
    model->registerAs("8ball");

    addNumberedDocs(1, 100);
    Retained<Query> query{ store->compileQuery(json5(
                                "{'WHAT': [['.num'], ['PREDICTION()', '8ball', ['.num']]]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 100);
    while (e->next()) {
        Log("%lld : %s", e->columns()[0]->asInt(), e->columns()[1]->toJSONString().c_str());
    }

    PredictiveModel::unregister("8ball");
}

#endif // COUCHBASE_ENTERPRISE
