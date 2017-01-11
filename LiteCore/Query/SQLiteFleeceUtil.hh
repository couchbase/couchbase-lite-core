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

    // SQLite value subtypes for tagging blobs as Fleece
    static const int kFleeceDataSubtype     = 0x66;   // Blob contains encoded Fleece data
    static const int kFleecePointerSubtype  = 0x67;   // Blob contains a raw Value* (4 or 8 bytes)


    // What the user_data of a registered function points to
    struct fleeceFuncContext {
        DataFile::FleeceAccessor accessor;
        fleece::SharedKeys *sharedKeys;
    };


    // Returns the data of a SQLite blob value as a slice
    static inline slice valueAsSlice(sqlite3_value *arg) noexcept {
        const void *blob = sqlite3_value_blob(arg); // must be called _before_ sqlite3_value_bytes
        return slice(blob, sqlite3_value_bytes(arg));
    }

    // Returns the data of a SQLite string value as a slice
    static inline slice valueAsStringSlice(sqlite3_value *arg) noexcept {
        auto blob = sqlite3_value_text(arg); // must be called _before_ sqlite3_value_bytes
        return slice(blob, sqlite3_value_bytes(arg));
    }


    const fleece::Value* fleeceParam(sqlite3_context*, sqlite3_value *arg) noexcept;

    int evaluatePath(slice path, fleece::SharedKeys*, const fleece::Value **pValue) noexcept;

    void setResultFromValue(sqlite3_context*, const fleece::Value*) noexcept;
    void setResultFromValueType(sqlite3_context*, const fleece::Value*) noexcept;
    void setResultTextFromSlice(sqlite3_context*, slice) noexcept;
    void setResultBlobFromSlice(sqlite3_context*, slice) noexcept;
    bool setResultBlobFromEncodedValue(sqlite3_context*, const fleece::Value*);
}
