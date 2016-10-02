//
//  SQLiteFleeceFunctions.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLiteFleeceFunctions.hh"
#include "Fleece.hh"
#include "Path.hh"
#include <sqlite3.h>

using namespace fleece;
using namespace std;

namespace litecore {


    static void fl_value(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        try {
            // Now get the Fleece data last, to ensure it stays valid w/o having to copy:
            const void *fleeceStart = sqlite3_value_blob(argv[0]);
            slice fleeceData(fleeceStart, sqlite3_value_bytes(argv[0]));
            const Value *root = Value::fromTrustedData(fleeceData);
            if (!root) {
                sqlite3_result_error(ctx, "fl_value: invalid Fleece data", -1);
            }

            slice fleecePath(sqlite3_value_blob(argv[1]), sqlite3_value_bytes(argv[1]));

            const Value *val = Path::eval(fleecePath, root);
            if (val == nullptr) {
                sqlite3_result_null(ctx);
            } else {
                switch (val->type()) {
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
                    case kNull:
                    case kArray:
                    case kDict:
                    default:
                        sqlite3_result_null(ctx);
                        break;
                }
            }

        } catch (const std::exception &x) {
            sqlite3_result_error(ctx, "fl_value: exception!", -1);
        }
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
            { "fl_value",                 2, 0,   fl_value        },
        };

        for(i=0; i<sizeof(aFunc)/sizeof(aFunc[0]) && rc==SQLITE_OK; i++){
            rc = sqlite3_create_function(db, aFunc[i].zName, aFunc[i].nArg,
                                         SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                         (void*)&aFunc[i].flag,
                                         aFunc[i].xFunc, 0, 0);
        }

        return rc;
    }
    
}
