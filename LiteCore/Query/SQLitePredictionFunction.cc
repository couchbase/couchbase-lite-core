//
// SQLitePredictFunction.cc
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

#ifdef COUCHBASE_ENTERPRISE

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
            alloc_slice result = model->prediction((const Dict*)input, &error);
            if (!result) {
                if (error.code == 0) {
                    LogVerbose(QueryLog, "    ...prediction returned no result");
                    setResultBlobFromFleeceData(ctx, result);
                } else {
                    alloc_slice desc(c4error_getDescription(error));
                    LogToAt(QueryLog, Error, "Predictive model '%s' failed: %.*s",
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
        } catch (const std::exception &x) {
            sqlite3_result_error(ctx, "predictionFunc: exception!", -1);
        }
    }

    static void euclideanDistanceFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
        auto p1 = fleeceParam(ctx, argv[0]), p2 = fleeceParam(ctx, argv[1]);
        if (!p1 || !p2)
            return;
        auto a1 = p1->asArray(), a2 = p2->asArray();
        if (!a1 || !a2)
            return;
        Array::iterator i1(a1), i2(a2);
        if (i1.count() != i2.count())
            return;

        double dist = 0.0;
        for (; i1; ++i1, ++i2) {
            double d = i1.value()->asDouble() - i2.value()->asDouble();
            dist += d * d;
        }
        sqlite3_result_double(ctx, sqrt(dist));
    }

#endif

    const SQLiteFunctionSpec kPredictFunctionsSpec[] = {
#ifdef COUCHBASE_ENTERPRISE
        { "prediction",         -1, predictionFunc  },
        { "euclidean_distance",  2, euclideanDistanceFunc  },
#endif
        { }
    };


}
