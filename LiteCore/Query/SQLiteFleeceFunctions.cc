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
#include <regex>
#include <unordered_set>

using namespace fleece;
using namespace std;

namespace litecore {


    const Value* fleeceParam(sqlite3_context* ctx, sqlite3_value *arg) noexcept {
        slice fleece = valueAsSlice(arg);
        if (sqlite3_value_subtype(arg) == kFleecePointerSubtype) {
            // Data is just a Value* (4 or 8 bytes), so extract it:
            if (fleece.size == sizeof(Value*)) {
                return *(const Value**)fleece.buf;
            } else {
                sqlite3_result_error(ctx, "invalid Fleece pointer", -1);
                sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
                return nullptr;
            }
        } else {
            if (sqlite3_value_subtype(arg) != kFleeceDataSubtype) {
                // Pull the Fleece data out of a raw document body:
                auto funcCtx = (fleeceFuncContext*)sqlite3_user_data(ctx);
                if (funcCtx->accessor)
                    fleece = funcCtx->accessor(fleece);
            }
            if (!fleece)
                return Dict::kEmpty;             // No body; may be deleted rev
            const Value *root = Value::fromTrustedData(fleece);
            if (!root) {
                Warn("Invalid Fleece data in SQLite table");
                sqlite3_result_error(ctx, "invalid Fleece data", -1);
                sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
            }
            return root;
        }
    }


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
        auto sharedKeys = ((fleeceFuncContext*)sqlite3_user_data(ctx))->sharedKeys;
        int rc = evaluatePath(path, sharedKeys, &val);
        if (rc == SQLITE_OK)
            return val;
        sqlite3_result_error_code(ctx, rc);
        return nullptr;
    }
    
    static void aggregateNumericArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                        function<void(double, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;
                    for (Array::iterator item(root->asArray()); item; ++item)
                        op(item->asDouble(), stop);
                    
                    break;
                }
                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    sqlite3_result_zeroblob(ctx, 0);
                    return;
            }
        }
        
        if(stop) {
            return;
        }
    }

    static void aggregateArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                               function<void(const Value *, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;
                    
                    if(root->type() != valueType::kArray) {
                        sqlite3_result_zeroblob(ctx, 0);
                        return;
                    }
                    
                    for (Array::iterator item(root->asArray()); item; ++item)
                        op(item.value(), stop);
                    
                    break;
                }
                    
                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    sqlite3_result_zeroblob(ctx, 0);
                    return;
            }
            
            if(stop) {
                return;
            }
        }
    }

    void setResultFromValue(sqlite3_context *ctx, const Value *val) noexcept {
        if (val == nullptr) {
            sqlite3_result_null(ctx);
        } else {
            switch (val->type()) {
                case kNull:
                    // Fleece/JSON null isn't the same as a SQL null, which means 'missing value'.
                    // We can't add new data types to SQLite, but let's use an empty blob for null.
                    sqlite3_result_zeroblob(ctx, 0);
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
                case kString:
                    setResultTextFromSlice(ctx, val->asString());
                    break;
                case kData:
                    setResultBlobFromSlice(ctx, val->asString());
                    break;
                case kArray:
                case kDict:
                    setResultBlobFromEncodedValue(ctx, val);
                    break;
            }
        }
    }


    void setResultFromValueType(sqlite3_context *ctx, const Value *val) noexcept {
        sqlite3_result_int(ctx, (val ? val->type() : -1));
    }


    void setResultTextFromSlice(sqlite3_context *ctx, slice text) noexcept {
        if (text)
            sqlite3_result_text(ctx, (const char*)text.buf, (int)text.size, SQLITE_TRANSIENT);
        else
            sqlite3_result_null(ctx);
    }

    void setResultBlobFromSlice(sqlite3_context *ctx, slice blob) noexcept {
        if (blob)
            sqlite3_result_blob(ctx, blob.buf, (int)blob.size, SQLITE_TRANSIENT);
        else
            sqlite3_result_null(ctx);
    }


    bool setResultBlobFromEncodedValue(sqlite3_context *ctx, const Value *val) {
        try {
            Encoder enc;
            enc.writeValue(val);
            setResultBlobFromSlice(ctx, enc.extractOutput());
            sqlite3_result_subtype(ctx, kFleeceDataSubtype);
            return true;
        } catch (const bad_alloc&) {
            sqlite3_result_error_code(ctx, SQLITE_NOMEM);
        } catch (...) {
            sqlite3_result_error_code(ctx, SQLITE_ERROR);
        }
        return false;
    }


#pragma mark - REGULAR FUNCTIONS:


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


    // array_sum() function adds up numbers. Any argument that's a number will be added.
    // Any argument that's a Fleece array will have all numeric values in it added.
    static void fl_array_sum(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum](double num, bool& stop) {
            sum += num;
        });
        
        sqlite3_result_double(ctx, sum);
    }
    
    static void fl_array_avg(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        double count = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum, &count](double num, bool& stop) {
            sum += num;
            count++;
        });
        
        if(count == 0.0) {
            sqlite3_result_double(ctx, 0.0);
        } else {
            sqlite3_result_double(ctx, sum / count);
        }
    }
    
    static void fl_array_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        slice comparand = valueAsStringSlice(argv[1]);
        bool found = false;
        aggregateArrayOperation(ctx, argc, argv, [&comparand, &found](const Value* val, bool& stop) {
            if(val->toString().compare(comparand) == 0) {
                found = stop = true;
            }
        });
        
        sqlite3_result_int(ctx, found ? 1 : 0);
    }
    
    static void fl_array_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            if(val->type() != valueType::kNull) {
                count++;
            }
        });
        
        sqlite3_result_int64(ctx, count);
    }
    
    static void fl_array_length(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            count++;
        });
        
        sqlite3_result_int64(ctx, count);
    }
    
#pragma mark - NON-FLEECE FUNCTIONS:


    static void contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        auto arg1 = valueAsStringSlice(argv[1]);
        sqlite3_result_int(ctx, arg0.find(arg1).buf != nullptr);
    }
    
    static void regexp_like(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsStringSlice(argv[0]);
        auto arg1 = valueAsStringSlice(argv[1]);
        regex r((char *)arg1.buf);
        int result = regex_search((char *)arg0.buf, r) ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }
    
    static void execute_if_numeric(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                   function<void(const vector<double>&)> op) {
        vector<double> args;
        for(int i = 0; i < argc; i++) {
            auto arg = argv[i];
            switch(sqlite3_value_numeric_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root || root->type() != valueType::kNumber)
                        return;
                    
                    args.push_back(root->asDouble());
                    break;
                }
                case SQLITE_INTEGER:
                case SQLITE_FLOAT:
                    args.push_back(sqlite3_value_double(arg));
                    break;
                default:
                    sqlite3_result_error(ctx, "Invalid numeric value", SQLITE_MISMATCH);
                    return;
            }
        }
        
        op(args);
    }

    static void fl_sqrt(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, sqrt(nums[0]));
        });
    }
    
    static void fl_log(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, log10(nums[0]));
        });
    }
    
    static void fl_ln(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, log(nums[0]));
        });
    }
    
    static void fl_exp(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, exp(nums[0]));
        });
    }
    
    static void fl_floor(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, floor(nums[0]));
        });
    }
    
    static void fl_ceiling(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, ceil(nums[0]));
        });
    }
    
    static void fl_power(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        execute_if_numeric(ctx, argc, argv, [ctx](const vector<double>& nums) {
            sqlite3_result_double(ctx, pow(nums[0], nums[1]));
        });
    }
    
#pragma mark - REGISTRATION:


    int RegisterFleeceFunctions(sqlite3 *db,
                                DataFile::FleeceAccessor accessor,
                                fleece::SharedKeys *sharedKeys)
    {
        // Adapted from json1.c in SQLite source code
        int rc = SQLITE_OK;
        unsigned int i;
        static const struct {
            const char *zName;
            int nArg;
            void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
        } aFunc[] = {
            { "fl_value",          2, fl_value  },
            { "fl_exists",         2, fl_exists },
            { "fl_type",           2, fl_type },
            { "fl_count",          2, fl_count },
            { "fl_contains",      -1, fl_contains },

            { "array_avg",        -1, fl_array_avg },
            { "array_contains",   -1, fl_array_contains },
            { "array_count",      -1, fl_array_count },
            { "array_length",     -1, fl_array_length },
            { "array_sum",        -1, fl_array_sum },

            { "contains",          2, contains },
            { "regexp_like",       2, regexp_like },

            { "sqrt",              1, fl_sqrt },
            { "log",               1, fl_log },
            { "ln",                1, fl_ln },
            { "exp",               1, fl_exp },
            { "power",             2, fl_power },
            { "floor",             1, fl_floor },
            { "ceil",              1, fl_ceiling },
        };

        for(i=0; i<sizeof(aFunc)/sizeof(aFunc[0]) && rc==SQLITE_OK; i++){
            rc = sqlite3_create_function_v2(db,
                                            aFunc[i].zName,
                                            aFunc[i].nArg,
                                            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                            new fleeceFuncContext{accessor, sharedKeys},
                                            aFunc[i].xFunc, nullptr, nullptr,
                                            [](void *param) {delete (fleeceFuncContext*)param;});
        }
        return rc;
    }
    
}
