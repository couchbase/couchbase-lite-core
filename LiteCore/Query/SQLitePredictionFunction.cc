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

    static void predictionFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
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
        alloc_slice result = model->predict((const Dict*)input, &error);
        if (result || error.code == 0) {
            if (QueryLog.willLog(LogLevel::Verbose)) {
                if (result)
                    LogVerbose(QueryLog, "    ...prediction took %.3fms", st.elapsedMS());
                else
                    LogVerbose(QueryLog, "    ...prediction returned no result");
            }
            setResultBlobFromFleeceData(ctx, result);
        } else {
            alloc_slice desc(c4error_getDescription(error));
            LogToAt(QueryLog, Error, "Predictive model '%s' failed: %.*s",
                    name, SPLAT(desc));
            alloc_slice msg = c4error_getMessage(error);
            sqlite3_result_error(ctx, (const char*)msg.buf, (int)msg.size);
            return;
        }
    }

#endif

    const SQLiteFunctionSpec kPredictFunctionsSpec[] = {
#ifdef COUCHBASE_ENTERPRISE
        { "prediction",       2, predictionFunc  },
#endif
        { }
    };


}
