//
// SQLiteFleeceUtil.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include "SQLiteFleeceUtil.hh"
#include "SQLite_Internal.hh"
#include "RawRevTree.hh"
#include "UnicodeCollator.hh"
#include "Path.hh"
#include "Error.hh"
#include "Logging.hh"
#include <SQLiteCpp/Exception.h>
#include <sqlite3.h>
#include <cmath>

using namespace fleece;
using namespace fleece::impl;
using namespace std;

namespace litecore {


    const char* const kFleeceValuePointerType = "FleeceValue";


    slice valueAsDocBody(sqlite3_value *arg, bool &outCopied) {
        outCopied = false;
        auto type = sqlite3_value_type(arg);
        if (_usuallyFalse(type == SQLITE_NULL))
            return nullslice;             // No 'body' column; may be deleted doc
        DebugAssert(type == SQLITE_BLOB);
        DebugAssert(sqlite3_value_subtype(arg) == 0);
        auto fleece = valueAsSlice(arg);
        if (RawRevision::isRevTree(fleece)) {
            // This is a 2.x-format `body` column containing a revision tree, i.e. the document
            // has not yet been updated to 3.0 format. Extract the current revision's body:
            fleece = RawRevision::getCurrentRevBody(fleece);
            if (_usuallyFalse(size_t(fleece.buf) & 1)) {
                // Fleece data at odd addresses used to be allowed, and CBL 2.0/2.1 didn't 16-bit-align
                // revision data, so it could occur. Now that it's not allowed, we have to work around
                // this by copying the data to an even address. (#589)
                fleece = fleece.copy();
                outCopied = true;
            }
        }
        return fleece;
    }


    const Value* fleeceParam(sqlite3_context* ctx, sqlite3_value *arg, bool required) noexcept {
        switch (sqlite3_value_type(arg)) {
            case SQLITE_BLOB: {
                switch (sqlite3_value_subtype(arg)) {
                    case 0: {
                        const Value *root = Value::fromTrustedData(valueAsSlice(arg));
                        if (root)
                            return root;
                        break;
                    }
                    case kFleeceNullSubtype:
                        return Value::kNullValue;
                    default:
                        break;
                }
                break;
            }
            case SQLITE_NULL: {
                const Value *value = asFleeceValue(arg);
                if (value)
                    return value;
                break;
            }
            default:
                break;
        }
        if (required) {
            sqlite3_result_error(ctx, "invalid Fleece data", -1);
            sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
        }
        return nullptr;
    }


    int evaluatePath(slice path, const Value **pValue) noexcept {
        if (_usuallyFalse(!path.buf))
            return SQLITE_FORMAT;
        try {
            *pValue = Path::eval(path, *pValue);    // can throw!
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


    const Value* evaluatePathFromArg(sqlite3_context *ctx, sqlite3_value **argv, int argNo, const Value *root) {
        // Cache a pre-parsed Path object using SQLite's auxdata API:
        auto path = (Path*)sqlite3_get_auxdata(ctx, argNo);
        if (path) {
            return path->eval(root);
        } else {
            // No cached Path yet, so create one, use it & cache it:
            path = new Path(valueAsSlice(argv[argNo]).asString());
            const Value *result = path->eval(root);
            sqlite3_set_auxdata(ctx, argNo, path, [](void *auxdata) {
                delete (Path*)auxdata;
            });
            return result;
        }
    }


    QueryFleeceScope::QueryFleeceScope(sqlite3_context *ctx, sqlite3_value **argv)
    :Scope(valueAsDocBody(argv[0], _copied),
           ((fleeceFuncContext*)sqlite3_user_data(ctx))->sharedKeys)
    {
        if (_usuallyTrue(data().buf != nullptr)) {
            root = Value::fromTrustedData(data());
            if (_usuallyFalse(!root)) {
                Warn("Invalid Fleece data in SQLite table");
                error::_throw(error::CorruptRevisionData);
            }
        } else {
            root = Dict::kEmpty;             // No current revision body; may be deleted rev
        }
        if (_usuallyTrue(sqlite3_value_type(argv[1]) != SQLITE_NULL))
            root = evaluatePathFromArg(ctx, argv, 1, root);
    }


    QueryFleeceScope::~QueryFleeceScope() {
        if (_usuallyFalse(_copied)) {
            unregister();
            free((void*)data().buf);
        }
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


    static void releaseAllocSlice(void *buf) noexcept {
        alloc_slice::release({buf, 1});
    }


    void setResultTextFromSlice(sqlite3_context *ctx, slice text) noexcept {
        if (text) {
            sqlite3_result_text(ctx, (const char*)text.buf, (int)text.size, SQLITE_TRANSIENT);
        } else {
            sqlite3_result_null(ctx);
        }
    }

    
    void setResultTextFromSlice(sqlite3_context *ctx, alloc_slice text) noexcept {
        if (text) {
            // Don't copy the data; retain the alloc_slice till SQLite is done with the text
            text.retain();
            sqlite3_result_text(ctx, (const char*)text.buf, (int)text.size, &releaseAllocSlice);
        } else {
            sqlite3_result_null(ctx);
        }
    }


    void setResultBlobFromData(sqlite3_context *ctx, slice blob, int subtype) noexcept {
        if (blob) {
            // This copies the blob data into SQLite.
            sqlite3_result_blob(ctx, blob.buf, (int)blob.size, SQLITE_TRANSIENT);
            if (subtype)
                sqlite3_result_subtype(ctx, subtype);
        } else {
            sqlite3_result_null(ctx);
        }
    }


    void setResultBlobFromData(sqlite3_context *ctx, alloc_slice blob, int subtype) noexcept {
        if (blob) {
            // Don't copy the data; retain the alloc_slice till SQLite is done with the blob
            blob.retain();
            sqlite3_result_blob(ctx, blob.buf, (int)blob.size, &releaseAllocSlice);
            if (subtype)
                sqlite3_result_subtype(ctx, subtype);
        } else {
            sqlite3_result_null(ctx);
        }
    }


    bool setResultBlobFromEncodedValue(sqlite3_context *ctx, const fleece::impl::Value *val)
    noexcept
    {
        try {
            Encoder enc;
            enc.writeValue(val);
            setResultBlobFromFleeceData(ctx, enc.finish());
            return true;
        } catch (const bad_alloc&) {
            sqlite3_result_error_code(ctx, SQLITE_NOMEM);
        } catch (...) {
            sqlite3_result_error_code(ctx, SQLITE_ERROR);
        }
        return false;
    }


    void setResultFleeceNull(sqlite3_context *ctx) noexcept {
        // Fleece/JSON null isn't the same as a SQL null, which means 'missing value'.
        // We can't add new data types to SQLite, but let's use an empty blob for null
        // and tag it with a custom subtype.
        sqlite3_result_zeroblob(ctx, 0);
        sqlite3_result_subtype(ctx, kFleeceNullSubtype);
    }


    static void registerFunctionSpecs(sqlite3 *db,
                                      const fleeceFuncContext &context,
                                      const SQLiteFunctionSpec functions[])
    {
        for (auto fn = functions; fn->name; ++fn) {
            int rc = sqlite3_create_function_v2(db,
                                                fn->name,
                                                fn->argCount,
                                                SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                                new fleeceFuncContext(context),
                                                fn->function, fn->stepCallback, fn->finalCallback,
                                                [](void *param) {delete (fleeceFuncContext*)param;});
            if (rc != SQLITE_OK)
                throw SQLite::Exception(db, rc);
        }
    }


    void RegisterSQLiteFunctions(sqlite3 *db, fleeceFuncContext context)
    {
        registerFunctionSpecs(db, context, kFleeceFunctionsSpec);
        registerFunctionSpecs(db, context, kRankFunctionsSpec);
        registerFunctionSpecs(db, context, kN1QLFunctionsSpec);
#ifdef COUCHBASE_ENTERPRISE
        registerFunctionSpecs(db, context, kPredictFunctionsSpec);
#endif
        RegisterFleeceEachFunctions(db, context);

        // The functions registered below operate on virtual tables, not on the actual db,
        // so they should not use the db's Fleece accessor. That's why we clear it first.
        context.delegate = nullptr;
        registerFunctionSpecs(db, context, kFleeceNullAccessorFunctionsSpec);
    }


    // Given an argument containing the name of a collation, returns a CollationContext pointer.
    // If the argument doesn't exist, returns a default context (case-sensitive, Unicode-aware.)
    CollationContext& collationContextFromArg(sqlite3_context* ctx,
                                              int argc, sqlite3_value **argv,
                                              int argNo)
    {
        if (argNo < argc) {
            auto collCtx = (CollationContext*) sqlite3_get_auxdata(ctx, argNo);
            if (!collCtx) {
                Collation col;
                col.readSQLiteName((const char *)sqlite3_value_text(argv[argNo]));
                col.unicodeAware = true;
                collCtx = CollationContext::create(col).release();
                sqlite3_set_auxdata(ctx, argNo, collCtx,
                                    [](void *aux) { delete (CollationContext*)aux; });
            }
            return *collCtx;
        } else {
            static CollationContext *sDefaultCollCtx = [] {
                Collation col;
                col.unicodeAware = true;
                return CollationContext::create(col).release();
            } ();
            return *sDefaultCollCtx;
        }
    }

    enhanced_bool_t booleanValue(sqlite3_context* ctx, sqlite3_value *arg) {
        SQL.log(LogLevel::Debug, "sqlite booleanValue()");
        switch(sqlite3_value_type(arg)) {
            case SQLITE_NULL:
                SQL.log(LogLevel::Debug, "sqlite booleanValue() - SQLITE_NULL");
                return kMissing;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            {
                auto val = sqlite3_value_double(arg);
                SQL.log(LogLevel::Debug, "sqlite booleanValue NUMBER val = %f", val);
                return static_cast<enhanced_bool_t>(val != 0.0 && !std::isnan(val));
            }
            case SQLITE_TEXT:
            {
                SQL.log(LogLevel::Debug, "sqlite booleanValue() - SQLITE_TEXT");
                // Need to call sqlite3_value_text here?
                return static_cast<enhanced_bool_t>(sqlite3_value_bytes(arg) > 0);
            }
            case SQLITE_BLOB:
            {
                SQL.log(LogLevel::Debug, "sqlite booleanValue() - SQLITE_BLOB");
                auto fleece = fleeceParam(ctx, arg);
                if (fleece == nullptr) {
                    return kFalse;
                } else switch(fleece->type()) {
                    case valueType::kArray:
                        return static_cast<enhanced_bool_t>(fleece->asArray()->count() > 0);
                    case valueType::kData:
                        return static_cast<enhanced_bool_t>(fleece->asData().size > 0);
                    case valueType::kDict:
                        return static_cast<enhanced_bool_t>(fleece->asDict()->count() > 0);
                    case valueType::kNull:
                        return kJSONNull;
                    default:
                        // Other Fleece types never show up in blobs
                        return kFalse;
                }
            }
            default:
                SQL.log(LogLevel::Debug, "sqlite booleanValue() - DEFAULT");
                return kTrue;
        }
    }


}
