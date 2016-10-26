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
#include <sqlite3.h>

using namespace fleece;
using namespace std;

namespace litecore {


    int evaluatePath(slice path, SharedKeys *sharedKeys, const Value **pValue) noexcept {
        if (!path.buf)
            return SQLITE_FORMAT;
        try {
            *pValue = Path::eval(path, sharedKeys, *pValue);    // can throw!
            return SQLITE_OK;
        } catch (const error &error) {
            WarnError("Invalid property path `%.*s` in query (err %d)",
                      (int)path.size, (char*)path.buf, error.code);
            return SQLITE_ERROR;
        } catch (const bad_alloc&) {
            return SQLITE_NOMEM;
        } catch (...) {
            return SQLITE_ERROR;
        }
    }


    static const Value* evaluatePath(sqlite3_context *ctx, slice path, const Value *val) noexcept {
        auto sharedKeys = (SharedKeys*)sqlite3_user_data(ctx);
        int rc = evaluatePath(path, sharedKeys, &val);
        if (rc == SQLITE_OK)
            return val;
        sqlite3_result_error_code(ctx, rc);
        return nullptr;
    }


    void setResultFromValue(sqlite3_context *ctx, const Value *val) noexcept {
        if (val == nullptr) {
            sqlite3_result_null(ctx);
        } else {
            switch (val->type()) {
                case kNull:
                    sqlite3_result_null(ctx);
                    break;
                case kBoolean:
                    sqlite3_result_int(ctx, val->asBool());
                    break;
                case kNumber:
                    if (val->isInteger() && !val->isUnsigned())
                        sqlite3_result_int64(ctx, val->asInt());
                    else
                        sqlite3_result_double(ctx, val->asDouble());
                    break;
                case kString: {
                    slice str = val->asString();
                    sqlite3_result_text(ctx, (const char*)str.buf, (int)str.size,
                                        SQLITE_TRANSIENT);
                    break;
                }
                case kData:{
                    slice str = val->asString();
                    sqlite3_result_blob(ctx, str.buf, (int)str.size, SQLITE_TRANSIENT);
                    break;
                }
                case kArray:
                case kDict: {
                    // Encode dict/array as Fleece:
                    try {
                        Encoder enc;
                        enc.writeValue(val);
                        auto data = enc.extractOutput();
                        sqlite3_result_blob(ctx, data.buf, (int)data.size, SQLITE_TRANSIENT);
                    } catch (const bad_alloc&) {
                        sqlite3_result_error_code(ctx, SQLITE_NOMEM);
                    } catch (...) {
                        sqlite3_result_error_code(ctx, SQLITE_ERROR);
                    }
                    break;
                }
            }
        }
    }


    void setResultFromValueType(sqlite3_context *ctx, const Value *val) noexcept {
        sqlite3_result_int(ctx, (val ? val->type() : -1));
    }


#pragma mark - REGULAR FUNCTIONS:


    // fl_value(fleeceData, propertyPath) -> propertyValue
    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *root = fleeceParam(ctx, argv[0]);
            if (!root)
                return;
            setResultFromValue(ctx, evaluatePath(ctx, valueAsSlice(argv[1]), root));
        } catch (const std::exception &x) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }


    // fl_exists(fleeceData, propertyPath) -> 0/1
    static void fl_exists(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        const Value *val = evaluatePath(ctx, valueAsSlice(argv[1]), root);
        sqlite3_result_int(ctx, (val ? 1 : 0));
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
        if (val->type() == kArray)
            sqlite3_result_int(ctx, val->asArray()->count());
        else
            sqlite3_result_null(ctx);
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
                case SQLITE_TEXT:
                case SQLITE_BLOB: {
                    valueType type = (argType == SQLITE_TEXT) ? kString : kData;
                    slice blobVal;
                    blobVal.buf = sqlite3_value_blob(arg);
                    blobVal.size = sqlite3_value_bytes(arg);
                    for (Array::iterator j(array); j; ++j) {
                        if (j->type() == type && j->asString() == blobVal) {
                            ++found;
                            break;
                        }
                    }
                    break;
                }
                case SQLITE_NULL: {
                    for (Array::iterator j(array); j; ++j) {
                        if (j->type() == kNull) {
                            ++found;
                            break;
                        }
                    }
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


    int RegisterFleeceFunctions(sqlite3 *db, fleece::SharedKeys *sharedKeys) {
        // Adapted from json1.c in SQLite source code
        int rc = SQLITE_OK;
        unsigned int i;
        static const struct {
            const char *zName;
            int nArg;
            void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
        } aFunc[] = {
            { "fl_value",                 2,   fl_value  },
            { "fl_exists",                2,   fl_exists },
            { "fl_type",                  2,   fl_type },
            { "fl_count",                 2,   fl_count },
            { "fl_contains",             -1,   fl_contains },
        };

        for(i=0; i<sizeof(aFunc)/sizeof(aFunc[0]) && rc==SQLITE_OK; i++){
            rc = sqlite3_create_function(db,
                                         aFunc[i].zName,
                                         aFunc[i].nArg,
                                         SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                         sharedKeys,
                                         aFunc[i].xFunc, nullptr, nullptr);
        }
        return rc;
    }
    
}
