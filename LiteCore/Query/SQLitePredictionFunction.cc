#ifdef COUCHBASE_ENTERPRISE

//
// SQLitePredictFunction.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
//  COUCHBASE LITE ENTERPRISE EDITION
//
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

#include "SQLiteFleeceUtil.hh"
#include "PredictiveModel.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "HeapValue.hh"
#include "Stopwatch.hh"
#include <sqlite3.h>
#include <string>


namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace fleece::impl;

    static void predictionFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
        try {
            auto name = (const char*)sqlite3_value_text(argv[0]);
            auto model = PredictiveModel::named(name);
            if (!model) {
                string msg = format("Unknown ML model name '%s'", name);
                sqlite3_result_error(ctx, msg.c_str(), -1);
                return;
            }

            const Value *input = fleeceParam(ctx, argv[1], false);
            if (!input || input->type() != kDict) {
                if (!input && sqlite3_value_type(argv[1]) == SQLITE_NULL)
                    sqlite3_result_null(ctx);
                else
                    sqlite3_result_error(ctx, "Parameter of prediction() must be a dictionary", -1);
                return;
            }

            Stopwatch st;
            if (QueryLog.willLog(LogLevel::Verbose)) {
                auto json = input->toJSONString();
                if (json.size() > 200)
                    json = json.substr(0, 200) + "..."; // suppress huge base64 image data dumps
                LogVerbose(QueryLog, "calling prediction(\"%s\", %s)", name, json.c_str());
                st.start();
            }

            C4Error error = {};
            alloc_slice result = model->prediction((const Dict*)input, getDBDelegate(ctx), &error);
            if (!result) {
                if (error.code == 0) {
                    LogVerbose(QueryLog, "    ...prediction returned no result");
                    setResultBlobFromFleeceData(ctx, result);
                } else {
                    alloc_slice desc(c4error_getDescription(error));
                    LogError(QueryLog, "Predictive model '%s' failed: %.*s",
                            name, SPLAT(desc));
                    alloc_slice msg = c4error_getMessage(error);
                    sqlite3_result_error(ctx, (const char*)msg.buf, (int)msg.size);
                }
                return;
            }

            LogVerbose(QueryLog, "    ...prediction took %.3fms", st.elapsedMS());

            if (argc < 3) {
                setResultBlobFromFleeceData(ctx, result);
            } else {
                const Value *val = Value::fromTrustedData(result);
                val = evaluatePathFromArg(ctx, argv, 2, val);
                setResultFromValue(ctx, val);
            }
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "predictionFunc: exception!", -1);
        }
    }


    // Creates Fleece array iterators on the 1st two parameters of the function.
    static bool getArrays(sqlite3_context *ctx, sqlite3_value **argv,
                          Array::iterator &i1, Array::iterator &i2)
    {
        auto p1 = fleeceParam(ctx, argv[0], false), p2 = fleeceParam(ctx, argv[1], false);
        if (!p1 || !p2)
            return false;
        auto a1 = p1->asArray(), a2 = p2->asArray();
        if (!a1 || !a2)
            return false;
        i1 = Array::iterator(a1);
        i2 = Array::iterator(a2);
        return (i1.count() == i2.count());
    }


    // https://en.wikipedia.org/wiki/Euclidean_distance
    static void euclidean_distance(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
        Array::iterator i1(nullptr), i2(nullptr);
        if (!getArrays(ctx, argv, i1, i2))
            return;

        double dist = 0.0;
        for (; i1; ++i1, ++i2) {
            double d = i1.value()->asDouble() - i2.value()->asDouble();
            dist += d * d;
        }
        
        // Optional 3rd param raises result to that power. (Useful for squared-Euclidean distance.)
        if (argc < 3) {
            dist = sqrt(dist);
        } else {
            double power = sqlite3_value_double(argv[2]);
            if (power != 2.0)
                dist = pow(sqrt(dist), power);
        }
        
        sqlite3_result_double(ctx, dist);
    }


    // https://en.wikipedia.org/wiki/Cosine_similarity
    static void cosine_distance(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
        Array::iterator i1(nullptr), i2(nullptr);
        if (!getArrays(ctx, argv, i1, i2))
            return;

        double aa = 0.0, ab = 0.0, bb = 0.0;
        for (; i1; ++i1, ++i2) {
            double a = i1.value()->asDouble(), b = i2.value()->asDouble();
            aa += a * a;
            ab += a * b;
            bb += b * b;
        }
        double dist =  1.0 - ab / sqrt(aa * bb);
        sqlite3_result_double(ctx, dist);
    }


    const SQLiteFunctionSpec kPredictFunctionsSpec[] = {
        { "prediction",         -1, predictionFunc  },
        { "euclidean_distance", -1, euclidean_distance  },
        { "cosine_distance",     2, cosine_distance  },
        { }
    };

}

#endif
