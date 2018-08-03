//
// SQLiteFleeceUtil.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include <SQLiteCpp/Exception.h>
#include <sqlite3.h>

using namespace fleece;
using namespace std;

namespace litecore {


    const char* const kFleeceValuePointerType = "FleeceValue";


    const Value* fleeceDocRoot(sqlite3_context* ctx, sqlite3_value *arg) noexcept {
        auto type = sqlite3_value_type(arg);
        if (type == SQLITE_NULL)
            return Dict::kEmpty;             // No 'body' column; may be deleted doc
        Assert(type == SQLITE_BLOB);
        Assert(sqlite3_value_subtype(arg) == 0);
        slice fleece = valueAsSlice(arg);
        auto funcCtx = (fleeceFuncContext*)sqlite3_user_data(ctx);
        fleece = funcCtx->accessor(fleece);
        if (!fleece)
            return Dict::kEmpty;             // No current revision body; may be deleted rev
        const Value *root = Value::fromTrustedData(fleece);
        if (!root) {
            Warn("Invalid Fleece data in SQLite table");
            sqlite3_result_error(ctx, "invalid Fleece data", -1);
            sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
        }
        return root;
    }


    const Value* fleeceParam(sqlite3_context* ctx, sqlite3_value *arg) noexcept {
        const Value *value = asFleeceValue(arg);
        if (value)
            return value;
        DebugAssert(sqlite3_value_type(arg) == SQLITE_BLOB);
        slice fleece = valueAsSlice(arg);
        switch (sqlite3_value_subtype(arg)) {
            case kFleeceDataSubtype: {
                if (!fleece)
                    return Dict::kEmpty;             // No body; may be deleted rev
                const Value *root = Value::fromTrustedData(fleece);
                if (root)
                    return root;
                break;
            }
            case kFleeceNullSubtype:
                return Value::kNullValue;
            default:
                break;
        }
        sqlite3_result_error(ctx, "invalid Fleece data", -1);
        sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
        return nullptr;
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


    bool evaluatePathFromArgs(sqlite3_context *ctx, sqlite3_value **argv, bool isDocBody, const Value* *outValue) {
        const Value *val = isDocBody ? fleeceDocRoot(ctx, argv[0]) : fleeceParam(ctx, argv[0]);
        if (!val)
            return false;

        // Cache a pre-parsed Path object using SQLite's auxdata API:
        auto path = (Path*)sqlite3_get_auxdata(ctx, 1);
        if (path) {
            *outValue = path->eval(val);
        } else {
            // No cached Path yet, so create one, use it & cache it:
            path = new Path(valueAsSlice(argv[1]).asString(), getSharedKeys(ctx));
            *outValue = path->eval(val);
            sqlite3_set_auxdata(ctx, 1, path, [](void *auxdata) {
                delete (Path*)auxdata;
            });
        }
        return true;
    }


    void setResultFromValue(sqlite3_context *ctx, const Value *val) noexcept {
        if (val == nullptr) {
            sqlite3_result_null(ctx);
        } else {
            switch (val->type()) {
                case kNull:
                    setResultFleeceNull(ctx);
                    break;
                case kBoolean:
                    sqlite3_result_int(ctx, val->asBool());
                    sqlite3_result_subtype(ctx, kFleeceIntBoolean);
                    break;
                case kNumber:
                    if (val->isInteger())
                        if(val->isUnsigned()) {
                            sqlite3_result_int64(ctx, val->asUnsigned());
                            sqlite3_result_subtype(ctx, kFleeceIntUnsigned);
                        } else {
                            sqlite3_result_int64(ctx, val->asInt());
                        }
                    else
                        sqlite3_result_double(ctx, val->asDouble());
                    break;
                case kString:
                    setResultTextFromSlice(ctx, val->asString());
                    break;
                case kData:
                case kArray:
                case kDict:
                    setResultBlobFromEncodedValue(ctx, val);
                    break;
            }
        }
    }


    void setResultTextFromSlice(sqlite3_context *ctx, slice text) noexcept {
        if (text)
            sqlite3_result_text(ctx, (const char*)text.buf, (int)text.size, SQLITE_TRANSIENT);
        else
            sqlite3_result_null(ctx);
    }

    
    void setResultBlobFromFleeceData(sqlite3_context *ctx, slice blob) noexcept {
        if (blob) {
            sqlite3_result_blob(ctx, blob.buf, (int)blob.size, SQLITE_TRANSIENT);
            sqlite3_result_subtype(ctx, kFleeceDataSubtype);
        } else {
            sqlite3_result_null(ctx);
        }
    }


    bool setResultBlobFromEncodedValue(sqlite3_context *ctx, const Value *val) {
        try {
            SharedKeys* sk = getSharedKeys(ctx);
            Encoder enc;
            enc.setSharedKeys(sk);
            enc.writeValue(val, sk);
            setResultBlobFromFleeceData(ctx, enc.extractOutput());
            return true;
        } catch (const bad_alloc&) {
            sqlite3_result_error_code(ctx, SQLITE_NOMEM);
        } catch (...) {
            sqlite3_result_error_code(ctx, SQLITE_ERROR);
        }
        return false;
    }


    void setResultFleeceNull(sqlite3_context *ctx) {
        // Fleece/JSON null isn't the same as a SQL null, which means 'missing value'.
        // We can't add new data types to SQLite, but let's use an empty blob for null
        // and tag it with a custom subtype.
        sqlite3_result_zeroblob(ctx, 0);
        sqlite3_result_subtype(ctx, kFleeceNullSubtype);
    }


    static void registerFunctionSpecs(sqlite3 *db,
                                      DataFile::FleeceAccessor accessor,
                                      fleece::SharedKeys *sharedKeys,
                                      const SQLiteFunctionSpec functions[])
    {
        if (!accessor)
            accessor = [](slice data) {return data;};
        for (auto fn = functions; fn->name; ++fn) {
            int rc = sqlite3_create_function_v2(db,
                                                fn->name,
                                                fn->argCount,
                                                SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                                new fleeceFuncContext{accessor, sharedKeys},
                                                fn->function, fn->stepCallback, fn->finalCallback,
                                                [](void *param) {delete (fleeceFuncContext*)param;});
            if (rc != SQLITE_OK)
                throw SQLite::Exception(db, rc);
        }
    }


    void RegisterSQLiteFunctions(sqlite3 *db,
                                 DataFile::FleeceAccessor accessor,
                                 fleece::SharedKeys *sharedKeys)
    {
        registerFunctionSpecs(db, accessor, sharedKeys, kFleeceFunctionsSpec);
        registerFunctionSpecs(db, accessor, sharedKeys, kRankFunctionsSpec);
        registerFunctionSpecs(db, accessor, sharedKeys, kN1QLFunctionsSpec);
        RegisterFleeceEachFunctions(db, accessor, sharedKeys);
    }

}
