//
// SQLiteFleeceFunctions.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Path.hh"
#include "Error.hh"
#include "Logging.hh"

using namespace fleece;
using namespace std;

namespace litecore {

    
    // Core SQLite functions for accessing values inside Fleece blobs.


    // fl_value(body, propertyPath) -> propertyValue
    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *val;
            if (!evaluatePathFromArgs(ctx, argv, true, &val))
                return;
            setResultFromValue(ctx, val);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }

    // fl_nested_value(fleeceData, propertyPath) -> propertyValue
    static void fl_nested_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *val;
            if (!evaluatePathFromArgs(ctx, argv, false, &val))
                return;
            setResultFromValue(ctx, val);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }


    // fl_root(body) -> fleeceData
    static void fl_root(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // Pull the Fleece data out of a raw document body:
        auto funcCtx = (fleeceFuncContext*)sqlite3_user_data(ctx);
        slice fleece = funcCtx->accessor(valueAsSlice(argv[0]));
        setResultBlobFromFleeceData(ctx, fleece);
    }

    // fl_exists(body, propertyPath) -> 0/1
    static void fl_exists(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *val;
        if (!evaluatePathFromArgs(ctx, argv, true, &val))
            return;
        sqlite3_result_int(ctx, (val ? 1 : 0));
        sqlite3_result_subtype(ctx, kFleeceIntBoolean);
    }

    
    // fl_count(body, propertyPath) -> int
    static void fl_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *val;
        if (!evaluatePathFromArgs(ctx, argv, true, &val))
            return;
        switch (val->type()) {
            case kArray:
                sqlite3_result_int(ctx, val->asArray()->count());
                break;
            case kDict:
                sqlite3_result_int(ctx, val->asDict()->count());
                break;
            default:
                sqlite3_result_null(ctx);
                break;
        }
    }


    // fl_contains(body, propertyPath, all?, value1, ...) -> 0/1
    static void fl_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        if (argc < 4) {
            sqlite3_result_error(ctx, "fl_contains: too few arguments", -1);
            return;
        }
        const Value *val;
        if (!evaluatePathFromArgs(ctx, argv, true, &val))
            return;
        const Array *array = val ? val->asArray() : nullptr;
        if (!array) {
            sqlite3_result_int(ctx, 0);
            return;
        }

        int found = 0, needed = 1;
        if (sqlite3_value_int(argv[2]) != 0)    // 'all' flag
            needed = (argc - 3);

        for (int i = 3; i < argc; ++i) {
            auto arg = argv[i];
            auto argType = sqlite3_value_type(arg);
            switch (argType) {
                case SQLITE_INTEGER: {
                    int64_t n = sqlite3_value_int64(arg);
                    for (Array::iterator j(array); j; ++j) {
                        if (j->type() == kNumber && j->isInteger() && j->asInt() == n) {
                            ++found;
                            break;
                        }
                    }
                    break;
                }
                case SQLITE_FLOAT: {
                    double n = sqlite3_value_double(arg);
                    for (Array::iterator j(array); j; ++j) {
                        if (j->type() == kNumber && j->asDouble() == n) {   //TODO: Approx equal?
                            ++found;
                            break;
                        }
                    }
                    break;
                }
                case SQLITE_BLOB:
                    if (sqlite3_value_bytes(arg) == 0) {
                        // A zero-length blob represents a Fleece/JSON 'null'.
                        for (Array::iterator j(array); j; ++j) {
                            if (j->type() == kNull) {
                                ++found;
                                break;
                            }
                        }
                        break;
                    }
                    // ... else fall through to match blobs:
                case SQLITE_TEXT: {
                    valueType type = (argType == SQLITE_TEXT) ? kString : kData;
                    const void *blob = sqlite3_value_blob(arg);
                    slice blobVal(blob, sqlite3_value_bytes(arg));
                    for (Array::iterator j(array); j; ++j) {
                        if (j->type() == type && j->asString() == blobVal) {
                            ++found;
                            break;
                        }
                    }
                    break;
                }
                case SQLITE_NULL: {
                    // A SQL null doesn't match anything
                    break;
                }
            }
            if (found >= needed) {
                sqlite3_result_int(ctx, 1);
                return;
            }
        }
        sqlite3_result_int(ctx, 0);
    }


    // fl_result(value) -> value suitable for use as a result column
    // Primarily what this does is change the various custom blob subtypes into Fleece containers
    // that can be read by SQLiteQueryRunner::encodeColumn().
    static void fl_result(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            auto arg = argv[0];
            if (sqlite3_value_type(arg) == SQLITE_BLOB) {
                switch (sqlite3_value_subtype(arg)) {
                    case kFleecePointerSubtype:
                    case kFleeceNullSubtype: {
                        auto value = fleeceParam(ctx, arg);
                        if (!value)
                            return;
                        setResultBlobFromEncodedValue(ctx, value);
                        break;
                    }
                    case kFleeceDataSubtype:
                        sqlite3_result_value(ctx, arg);
                        break;
                    default: {
                        // A plain blob/data value has to be wrapped in a Fleece container to avoid
                        // misinterpretation, since SQLiteQueryRunner will assume all blob results
                        // are Fleece containers.
                        Encoder enc;
                        enc.writeData(valueAsSlice(arg));
                        setResultBlobFromFleeceData(ctx, enc.extractOutput());
                        break;
                    }
                }
            } else {
                sqlite3_result_value(ctx, arg);
            }
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_result: exception!", -1);
        }
    }


#pragma mark - REGISTRATION:


    const SQLiteFunctionSpec kFleeceFunctionsSpec[] = {
        { "fl_root",           1, fl_root },
        { "fl_value",          2, fl_value },
        { "fl_nested_value",   2, fl_nested_value },
        { "fl_exists",         2, fl_exists },
        { "fl_count",          2, fl_count },
        { "fl_contains",      -1, fl_contains },
        { "fl_result",         1, fl_result },
        { }
    };

}
