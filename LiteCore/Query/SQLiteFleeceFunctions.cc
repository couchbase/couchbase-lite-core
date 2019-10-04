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
#include "DeepIterator.hh"
#include <sstream>

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
            setResultBlobFromFleeceData(ctx, fleeceAccessor(ctx, body));
            return;
        }
        // If arg isn't a blob, check if it's a tagged Fleece pointer:
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

    // fl_blob(body, propertyPath) -> blob data
    static void fl_blob(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if (!scope.root)
                return;
            const Dict *blobDict = scope.root->asDict();
            if (!blobDict)
                return;
            // Read the blob data:
            auto delegate = getDBDelegate(ctx);
            if (!delegate)
                return;
            alloc_slice data;
            try {
                data = delegate->blobAccessor(blobDict);
            } catch (const std::exception &x) {
                // ignore exception; just return 'missing'
            }
            setResultBlobFromData(ctx, data);
        } catch (const std::exception &x) {
            sqlite3_result_error(ctx, "unexpected error reading blob", -1);
        }
    }

    // fl_nested_value(fleeceData, propertyPath) -> propertyValue
    static void fl_nested_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            const Value *val = fleeceParam(ctx, argv[0], false);
            if (!val) {
                sqlite3_result_null(ctx);
                return;
            }
            val = evaluatePathFromArg(ctx, argv, 1, val);
            setResultFromValue(ctx, val);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_nested_value: exception!", -1);
        }
    }

    static void handle_fts_value(const Value* data, stringstream& result) {
        if(_usuallyFalse(!data)) {
            Warn("Null value received in handle_fts_value");
            return;
        }

        switch(data->type()) {
            case kBoolean:
                result << (data->asBool() ? "T" : "F");
                break;
            case kNumber:
            {
                const alloc_slice stringified = data->toString();
                result << stringified.asString();
                break;
            }
            case kString:
                result << data->asString().asString();
                break;
            default:
                break;
        }
    }

    // fl_fts_value(body, propertyPath) -> blob data
    static void fl_fts_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if(scope.root == nullptr) {
                return;
            } 

            stringstream result;
            for (DeepIterator j(scope.root); j; ++j) {
                handle_fts_value(j.value(), result);
                result << " ";
            }

            const string resultStr = result.str();
            setResultTextFromSlice(ctx, slice(resultStr.substr(0, resultStr.size() - 1)));
        } catch(const std::exception &) {
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
                    case 0:
                        sqlite3_result_value(ctx, arg);
                        break;
                    case kPlainBlobSubtype: {
                        // A plain blob/data value has to be wrapped in a Fleece container to avoid
                        // misinterpretation, since SQLiteQueryRunner will assume all blob results
                        // are Fleece containers.
                        Encoder enc;
                        enc.writeData(valueAsSlice(arg));
                        setResultBlobFromFleeceData(ctx, enc.finish());
                        break;
                    }
                    default:
                        Assert(false, "Invalid blob subtype");
                }
            } else {
                sqlite3_result_value(ctx, arg);
            }
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "fl_result: exception!", -1);
        }
    }


    static void fl_null(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_result_zeroblob(ctx, 0);
        sqlite3_result_subtype(ctx, kFleeceNullSubtype);
    }

    static void fl_bool(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_result_int(ctx, sqlite3_value_int(argv[0]) != 0);
        sqlite3_result_subtype(ctx, kFleeceIntBoolean);
    }


#pragma mark - CONTAINS()


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


#pragma mark - ARRAY() and OBJECT()


    // writes a SQLite arg value to a Fleece Encoder. Returns false on failure
    static bool writeSQLiteValue(sqlite3_context* ctx, sqlite3_value *arg, slice key, Encoder &enc) {
        auto type = sqlite3_value_type(arg);
        if (key && type != SQLITE_NULL)
            enc.writeKey(key);
        switch (type) {
            case SQLITE_INTEGER: {
                auto intVal = sqlite3_value_int64(arg);
                if(sqlite3_value_subtype(arg) == kFleeceIntBoolean)
                    enc.writeBool(intVal != 0);
                else
                    enc.writeInt(intVal);
                break;
            }
            case SQLITE_FLOAT:
                enc.writeDouble( sqlite3_value_double(arg) );
                break;
            case SQLITE_TEXT:
                enc.writeString(valueAsStringSlice(arg));
                break;
            case SQLITE_BLOB: {
                switch (sqlite3_value_subtype(arg)) {
                    case 0: {
                        const Value *value = fleeceParam(ctx, arg);
                        if (!value)
                            return false; // error occurred
                        enc.writeValue(value);
                        break;
                    }
                    case kFleeceNullSubtype:
                        enc.writeNull();
                        break;
                    case kPlainBlobSubtype:
                        enc.writeData(valueAsSlice(arg));
                        break;
                    default:
                        sqlite3_result_error(ctx, "internal error: unknown blob subtype", -1);
                        return false;
                }
                break;
            }
            case SQLITE_NULL: {
                const Value *value = asFleeceValue(arg);
                if (value) {
                    if (key)
                        enc.writeKey(key);
                    enc.writeValue(value);
                }
                break;
            }
        }
        return true;
    }


    static void array_of(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        Encoder enc;
        enc.beginArray(argc);
        for (int i = 0; i < argc; i++) {
            if (!writeSQLiteValue(ctx, argv[i], nullslice, enc))
                return;
        }
        enc.endArray();
        setResultBlobFromFleeceData(ctx, enc.finish());
    }


    static void dict_of(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        if (argc % 2) {
            sqlite3_result_error(ctx, "object() must have an even arg count", -1);
            return;
        }
        Encoder enc;
        enc.beginDictionary(argc / 2);
        for (int i = 0; i < argc; i += 2) {
            slice key = valueAsStringSlice(argv[i]);
            if (!key) {
                sqlite3_result_error(ctx, "invalid key arg to object()", -1);
                return;
            }
            if (!writeSQLiteValue(ctx, argv[i+1], key, enc))
                return;
        }
        enc.endDictionary();
        setResultBlobFromFleeceData(ctx, enc.finish());
    }

    static void fl_like(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        Collation col;
        col.unicodeAware = true;
        if(argc > 2) {
            col.readSQLiteName((const char *)sqlite3_value_text(argv[2]));
        }

        const int likeResult = LikeUTF8(valueAsStringSlice(argv[0]), valueAsStringSlice(argv[1]), col);
        sqlite3_result_int(ctx, likeResult == kLikeMatch);
    }

#pragma mark - REGISTRATION:


    const SQLiteFunctionSpec kFleeceFunctionsSpec[] = {
        { "fl_root",           1, fl_root },
        { "fl_value",          2, fl_value },
        { "fl_nested_value",   2, fl_nested_value },
        { "fl_fts_value",      2, fl_fts_value },
        { "fl_blob",           2, fl_blob },
        { "fl_exists",         2, fl_exists },
        { "fl_count",          2, fl_count },
        { "fl_contains",       3, fl_contains },
        { "fl_result",         1, fl_result },
        { "fl_null",           0, fl_null },
        { "fl_bool",           1, fl_bool },
        { "array_of",         -1, array_of },
        { "dict_of",          -1, dict_of },
        { "fl_like",           2, fl_like },
        { "fl_like",           3, fl_like },
        { }
    };

    const SQLiteFunctionSpec kFleeceNullAccessorFunctionsSpec[] = {
        { "fl_unnested_value",-1, fl_unnested_value },
        { }
    };
}
