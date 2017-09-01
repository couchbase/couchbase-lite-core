//
//  SQLiteFleeceFunctions.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Path.hh"
#include "Error.hh"
#include "Logging.hh"

using namespace fleece;
using namespace std;

namespace litecore {

    
    // Core SQLite functions for accessing values inside Fleece blobs.


    // fl_value(fleeceData, propertyPath) -> propertyValue
    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *root = fleeceParam(ctx, argv[0]);
            if (!root)
                return;
            
            setResultFromValue(ctx, evaluatePath(ctx, valueAsSlice(argv[1]), root));
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }

    static void fl_root(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        slice fleece = valueAsSlice(argv[0]);
        // Pull the Fleece data out of a raw document body, if necessary:
        auto funcCtx = (fleeceFuncContext*)sqlite3_user_data(ctx);
        if (funcCtx->accessor)
            fleece = funcCtx->accessor(fleece);
        
        setResultBlobFromSlice(ctx, fleece);
    }

    // fl_exists(fleeceData, propertyPath) -> 0/1
    static void fl_exists(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        
        const Value* propertyVal = evaluatePath(ctx, valueAsSlice(argv[1]), root);
        if(propertyVal == nullptr || propertyVal->type() != valueType::kArray) {
            sqlite3_result_int(ctx, 0);
            return;
        }
        
        auto val = propertyVal->asArray()->count() > 0 ? 1 : 0;
        sqlite3_result_int(ctx, val);
    }

    
    // fl_type(fleeceData, propertyPath) -> int  (fleece::valueType, or -1 for no value)
    static void fl_type(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        setResultFromValueType(ctx, evaluatePath(ctx, valueAsSlice(argv[1]), root));
    }

    
    // fl_count(fleeceData, propertyPath) -> int
    static void fl_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        const Value *val = evaluatePath(ctx, valueAsSlice(argv[1]), root);
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


    // fl_contains(fleeceData, propertyPath, all?, value1, ...) -> 0/1
    static void fl_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        if (argc < 4) {
            sqlite3_result_error(ctx, "fl_contains: too few arguments", -1);
            return;
        }
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        root = evaluatePath(ctx, valueAsSlice(argv[1]), root);
        if (!root)
            return;
        const Array *array = root->asArray();
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


#pragma mark - REGISTRATION:


    const SQLiteFunctionSpec kFleeceFunctionsSpec[] = {
        { "fl_root",           1, fl_root  },
        { "fl_value",          2, fl_value  },
        { "fl_exists",         2, fl_exists },
        { "fl_type",           2, fl_type },
        { "fl_count",          2, fl_count },
        { "fl_contains",      -1, fl_contains },
        { }
    };

}
