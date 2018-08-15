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
#include "fleece/Fleece.h"

using namespace fleece;
using namespace fleece::impl;
using namespace std;

namespace litecore {

    
    // Core SQLite functions for accessing values inside Fleece blobs.


    // fl_root(body) -> fleeceData
    static void fl_root(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        // Pull the Fleece data out of a raw document body:
        slice body = valueAsSlice(argv[0]);
        if (body) {
            DebugAssert(sqlite3_value_type(argv[0]) == SQLITE_BLOB);
            DebugAssert(sqlite3_value_subtype(argv[0]) == 0);
            auto funcCtx = (fleeceFuncContext*)sqlite3_user_data(ctx);
            slice fleece = funcCtx->accessor(body);
            setResultBlobFromFleeceData(ctx, fleece);
            return;
        }
        const Value *val = asFleeceValue(argv[0]);
        if (val) {
            sqlite3_result_pointer(ctx, (void*)val, kFleeceValuePointerType, nullptr);
        } else {
            DebugAssert(sqlite3_value_type(argv[0]) == SQLITE_NULL);
            sqlite3_result_null(ctx);
        }
    }

    // fl_value(body, propertyPath) -> propertyValue
    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            setResultFromValue(ctx, scope.root);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }

    // fl_nested_value(fleeceData, propertyPath) -> propertyValue
    static void fl_nested_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *val = fleeceParam(ctx, argv[0]);
            if (!val)
                return;
            val = evaluatePathFromArg(ctx, argv, 1, val);
            setResultFromValue(ctx, val);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_nested_value: exception!", -1);
        }
    }

    // fl_unnested_value(unnestTableBody [, propertyPath]) -> propertyValue
    static void fl_unnested_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        DebugAssert(argc == 1 || argc == 2);
        sqlite3_value *body = argv[0];
        if (sqlite3_value_type(body) == SQLITE_BLOB) {
            // body is Fleece data:
            if (argc == 1)
                return fl_root(ctx, argc, argv);
            else
                return fl_value(ctx, argc, argv);
        } else {
            // body is a SQLite value; just return it
            if (argc == 1)
                sqlite3_result_value(ctx, body);
            else
                sqlite3_result_null(ctx);
        }
    }


    // fl_exists(body, propertyPath) -> 0/1
    static void fl_exists(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            sqlite3_result_int(ctx, (scope.root ? 1 : 0));
            sqlite3_result_subtype(ctx, kFleeceIntBoolean);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_exists: exception!", -1);
        }
    }

    
    // fl_count(body, propertyPath) -> int
    static void fl_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if (!scope.root) {
                sqlite3_result_null(ctx);
                return;
            }
            switch (scope.root->type()) {
                case kArray:
                    sqlite3_result_int(ctx, scope.root->asArray()->count());
                    break;
                case kDict:
                    sqlite3_result_int(ctx, scope.root->asDict()->count());
                    break;
                default:
                    sqlite3_result_null(ctx);
                    break;
            }
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_count: exception!", -1);
        }
    }


    // fl_contains(body, propertyPath, value) -> 0/1
    static void fl_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            collectionContainsImpl(ctx, scope.root, argv[2]);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_contains: exception!", -1);
        }
    }


    void collectionContainsImpl(sqlite3_context* ctx, const Value *collection, sqlite3_value *arg) {
        if (!collection || collection->type() < kArray) {
            sqlite3_result_zeroblob(ctx, 0); // JSON null
            return;
        }

        // Set up a predicate callback that will match the desired value:
        union target_t {
            int64_t i;
            double d;
            FLSlice s;
        };
        target_t target;
        valueType targetType;
        bool (*predicate)(const Value*, const target_t&);

        switch (sqlite3_value_type(arg)) {
            case SQLITE_INTEGER: {
                targetType = kNumber;
                target.i = sqlite3_value_int64(arg);
                predicate = [](const Value *v, const target_t &t) {return v->asInt() == t.i;};
                break;
            }
            case SQLITE_FLOAT: {
                targetType = kNumber;
                target.d = sqlite3_value_double(arg);
                predicate = [](const Value *v, const target_t &t) {return v->asDouble() == t.d;};
                break;
            }
            case SQLITE_TEXT: {
                targetType = kString;
                target.s = slice(sqlite3_value_blob(arg), sqlite3_value_bytes(arg));
                predicate = [](const Value *v, const target_t &t) {return v->asString() == slice(t.s);};
                break;
            }
            case SQLITE_BLOB: {
                if (sqlite3_value_bytes(arg) == 0) {
                    // A zero-length blob represents a Fleece/JSON 'null'.
                    sqlite3_result_zeroblob(ctx, 0); // JSON null
                    return;
                } else {
                    targetType = kData;
                    target.s = slice(sqlite3_value_blob(arg), sqlite3_value_bytes(arg));
                    predicate = [](const Value *v, const target_t &t) {return v->asData() == slice(t.s);};
                }
                break;
            }
            case SQLITE_NULL:
            default:{
                // A SQL null (MISSING) doesn't match anything
                sqlite3_result_null(ctx);
                return;
            }
        }

        // Now iterate the array/dict:
        bool found = false;
        if (collection->type() == kArray) {
            for (Array::iterator j(collection->asArray()); j; ++j) {
                auto val = j.value();
                if (val->type() == targetType && predicate(val, target)) {
                    found = true;
                    break;
                }
            }
        } else {
            for (Dict::iterator j(collection->asDict()); j; ++j) {
                auto val = j.value();
                if (val->type() == targetType && predicate(val, target)) {
                    found = true;
                    break;
                }
            }
        }
        sqlite3_result_int(ctx, found);
    }


    // fl_result(value) -> value suitable for use as a result column
    // Primarily what this does is change the various custom blob subtypes into Fleece containers
    // that can be read by SQLiteQueryRunner::encodeColumn().
    static void fl_result(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            auto arg = argv[0];
            const Value *value = asFleeceValue(arg);
            if (value) {
                setResultBlobFromEncodedValue(ctx, value);
            } else if (sqlite3_value_type(arg) == SQLITE_BLOB) {
                switch (sqlite3_value_subtype(arg)) {
                    case kFleeceNullSubtype: {
                        value = fleeceParam(ctx, arg);
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
                        setResultBlobFromFleeceData(ctx, enc.finish());
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
        { "fl_contains",       3, fl_contains },
        { "fl_result",         1, fl_result },
        { }
    };

    const SQLiteFunctionSpec kFleeceNullAccessorFunctionsSpec[] = {
        { "fl_unnested_value",-1, fl_unnested_value },
        { }
    };
}
