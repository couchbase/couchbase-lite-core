//
//  SQLiteFleeceUtil.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/25/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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


    const Value* evaluatePath(sqlite3_context *ctx, slice path, const Value *val) noexcept {
        auto sharedKeys = ((fleeceFuncContext*)sqlite3_user_data(ctx))->sharedKeys;
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


    static void registerFunctionSpecs(sqlite3 *db,
                                      DataFile::FleeceAccessor accessor,
                                      fleece::SharedKeys *sharedKeys,
                                      const SQLiteFunctionSpec functions[])
    {
        for (auto fn = functions; fn->name; ++fn) {
            int rc = sqlite3_create_function_v2(db,
                                                fn->name,
                                                fn->argCount,
                                                SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                                new fleeceFuncContext{accessor, sharedKeys},
                                                fn->function, nullptr, nullptr,
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
