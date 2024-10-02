//
// SQLitePredictFunction.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#ifdef COUCHBASE_ENTERPRISE
#    include "SQLiteFleeceUtil.hh"
#    include "PredictiveModel.hh"
#    include "Array.hh"
#    include "Logging.hh"
#    include "StringUtil.hh"
#    include "Stopwatch.hh"
#    include <sqlite3.h>
#    include <string>

namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace fleece::impl;

    // Implementation of N1QL function PREDICTION(NAME, INPUT, [PROPERTY]).
    // Calls the named PredictiveModel, passing it the INPUT dict, returning the output dict.
    // If PROPERTY is given, only that named property of the output dict is returned.
    static void predictionFunc(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        try {
            auto name  = (const char*)sqlite3_value_text(argv[0]);
            auto model = PredictiveModel::named(name);
            if ( !model ) {
                string msg = stringprintf("Unknown ML model name '%s'", name);
                sqlite3_result_error(ctx, msg.c_str(), -1);
                return;
            }

            const QueryFleeceParam input{ctx, argv[1], false};
            if ( !input || input->type() != kDict ) {
                if ( !input && sqlite3_value_type(argv[1]) == SQLITE_NULL ) sqlite3_result_null(ctx);
                else
                    sqlite3_result_error(ctx, "Parameter of prediction() must be a dictionary", -1);
                return;
            }

            Stopwatch st;
            if ( QueryLog.willLog(LogLevel::Verbose) ) {
                auto json = input->toJSONString();
                if ( json.size() > 200 ) json = json.substr(0, 200) + "...";  // suppress huge base64 image data dumps
                LogVerbose(QueryLog, "calling prediction(\"%s\", %s)", name, json.c_str());
                st.start();
            }

            C4Error     error     = {};
            const auto  inputDict = reinterpret_cast<const Dict*>(static_cast<const Value*>(input));
            alloc_slice result    = model->prediction(inputDict, getDBDelegate(ctx), &error);
            if ( !result ) {
                if ( error.code == 0 ) {
                    LogVerbose(QueryLog, "    ...prediction returned no result");
                    setResultBlobFromFleeceData(ctx, result);
                } else {
                    alloc_slice desc(c4error_getDescription(error));
                    LogError(QueryLog, "Predictive model '%s' failed: %.*s", name, SPLAT(desc));
                    alloc_slice msg = c4error_getMessage(error);
                    sqlite3_result_error(ctx, (const char*)msg.buf, (int)msg.size);
                }
                return;
            }

            LogVerbose(QueryLog, "    ...prediction took %.3fms", st.elapsedMS());

            if ( argc < 3 ) {
                setResultBlobFromFleeceData(ctx, result);
            } else {
                const Value* val = Value::fromTrustedData(result);
                val              = evaluatePathFromArg(ctx, argv, 2, val);
                setResultFromValue(ctx, val);
            }
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "predictionFunc: exception!", -1); }
    }

    // Creates Fleece array iterators on the 1st two parameters of the function.
    static bool getArrays(sqlite3_context* ctx, sqlite3_value** argv, Array::iterator& i1, Array::iterator& i2) {
        const QueryFleeceParam p1{ctx, argv[0], false};
        const QueryFleeceParam p2{ctx, argv[1], false};
        if ( !p1 || !p2 ) return false;
        auto a1 = p1->asArray(), a2 = p2->asArray();
        if ( !a1 || !a2 ) return false;
        i1 = Array::iterator(a1);
        i2 = Array::iterator(a2);
        return (i1.count() == i2.count());
    }

    // Implementation of N1QL function EUCLIDEAN_DISTANCE(ARRAY1, ARRAY2)
    // Given two arrays of numbers, returns their Euclidean distance:
    // https://en.wikipedia.org/wiki/Euclidean_distance
    // Returns NULL if args are not both arrays and of equal length.
    static void euclidean_distance(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        Array::iterator i1(nullptr), i2(nullptr);
        if ( !getArrays(ctx, argv, i1, i2) ) return;

        double dist = 0.0;
        for ( ; i1; ++i1, ++i2 ) {
            double d = i1.value()->asDouble() - i2.value()->asDouble();
            dist += d * d;
        }

        // Optional 3rd param raises result to that power. (Useful for squared-Euclidean distance.)
        if ( argc < 3 ) {
            dist = sqrt(dist);
        } else {
            double power = sqlite3_value_double(argv[2]);
            if ( power != 2.0 ) dist = pow(sqrt(dist), power);
        }

        sqlite3_result_double(ctx, dist);
    }

    // Implementation of N1QL function COSINE_DISTANCE(ARRAY1, ARRAY2)
    // Given two arrays of numbers, returns their cosine distance:
    // https://en.wikipedia.org/wiki/Cosine_similarity
    // Returns NULL if args are not both arrays and of equal length.
    static void cosine_distance(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) {
        Array::iterator i1(nullptr), i2(nullptr);
        if ( !getArrays(ctx, argv, i1, i2) ) return;

        double aa = 0.0, ab = 0.0, bb = 0.0;
        for ( ; i1; ++i1, ++i2 ) {
            double a = i1.value()->asDouble(), b = i2.value()->asDouble();
            aa += a * a;
            ab += a * b;
            bb += b * b;
        }
        double dist = 1.0 - ab / sqrt(aa * bb);
        sqlite3_result_double(ctx, dist);
    }

    const SQLiteFunctionSpec kPredictFunctionsSpec[] = {{"prediction", -1, predictionFunc},
                                                        {"euclidean_distance", -1, euclidean_distance},
                                                        {"cosine_distance", 2, cosine_distance},
                                                        {}};

}  // namespace litecore

#endif
