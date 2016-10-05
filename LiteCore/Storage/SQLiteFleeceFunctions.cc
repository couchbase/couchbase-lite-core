//
//  SQLiteFleeceFunctions.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLite_Internal.hh"
#include "Fleece.hh"
#include "Path.hh"
#include <sqlite3.h>

using namespace fleece;
using namespace std;

namespace litecore {


    static inline slice valueAsSlice(sqlite3_value *arg) {
        return slice(sqlite3_value_blob(arg), sqlite3_value_bytes(arg));
    }

    
    static inline const Value* fleeceParam(sqlite3_context* ctx, sqlite3_value *arg) {
        const Value *root = Value::fromTrustedData(valueAsSlice(arg));
        if (!root) {
            sqlite3_result_error(ctx, "fl_value: invalid Fleece data", -1);
            sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
        }
        return root;
    }


    // fl_value(fleeceData, propertyPath) -> propertyValue
    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        try {
            const Value *root = fleeceParam(ctx, argv[0]);
            if (!root)
                return;
            const Value *val = Path::eval(valueAsSlice(argv[1]), root);
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
                        Encoder enc;
                        enc.writeValue(val);
                        auto data = enc.extractOutput();
                        sqlite3_result_blob(ctx, data.buf, (int)data.size, SQLITE_TRANSIENT);
                        break;
                    }
                }
            }

        } catch (const std::exception &x) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
    }


    // fl_exists(fleeceData, propertyPath) -> 0/1
    static void fl_exists(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        const Value *val = Path::eval(valueAsSlice(argv[1]), root);
        sqlite3_result_int(ctx, (val ? 1 : 0));
    }

    
    // fl_type(fleeceData, propertyPath) -> int  (fleece::valueType, or -1 for no value)
    static void fl_type(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        const Value *val = Path::eval(valueAsSlice(argv[1]), root);
        sqlite3_result_int(ctx, (val ? val->type() : -1));
    }

    
    // fl_count(fleeceData, propertyPath) -> int
    static void fl_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        const Value *root = fleeceParam(ctx, argv[0]);
        if (!root)
            return;
        const Value *val = Path::eval(valueAsSlice(argv[1]), root);
        if (val->type() == kArray)
            sqlite3_result_int(ctx, val->asArray()->count());
        else
            sqlite3_result_null(ctx);
    }


    int RegisterFleeceFunctions(sqlite3 *db) {
        // Adapted from json1.c in SQLite source code
        int rc = SQLITE_OK;
        unsigned int i;
        static const struct {
            const char *zName;
            int nArg;
            int flag;
            void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
        } aFunc[] = {
            { "fl_value",                 2, 0,   fl_value  },
            { "fl_exists",                2, 0,   fl_exists },
            { "fl_type",                  2, 0,   fl_type },
            { "fl_count",                 2, 0,   fl_count },
        };

        for(i=0; i<sizeof(aFunc)/sizeof(aFunc[0]) && rc==SQLITE_OK; i++){
            rc = sqlite3_create_function(db,
                                         aFunc[i].zName,
                                         aFunc[i].nArg,
                                         SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                         (void*)&aFunc[i].flag,
                                         aFunc[i].xFunc, nullptr, nullptr);
        }

        return rc;
    }
    
}
