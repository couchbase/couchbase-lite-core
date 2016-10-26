//
//  SQLiteFleeceUtil.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/7/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "Fleece.hh"
#include <sqlite3.h>


namespace litecore {

    static inline slice valueAsSlice(sqlite3_value *arg) {
        const void *blob = sqlite3_value_blob(arg); // must be called _before_ sqlite3_value_bytes
        return slice(blob, sqlite3_value_bytes(arg));
    }


    static inline const fleece::Value* fleeceParam(sqlite3_context* ctx, sqlite3_value *arg) {
        const fleece::Value *root = fleece::Value::fromTrustedData(valueAsSlice(arg));
        if (!root) {
            sqlite3_result_error(ctx, "invalid Fleece data", -1);
            sqlite3_result_error_code(ctx, SQLITE_MISMATCH);
        }
        return root;
    }

    int evaluatePath(slice path, fleece::SharedKeys*, const fleece::Value **pValue) noexcept;
    void setResultFromValue(sqlite3_context *ctx, const fleece::Value *val) noexcept;
    void setResultFromValueType(sqlite3_context *ctx, const fleece::Value *val) noexcept;


}
