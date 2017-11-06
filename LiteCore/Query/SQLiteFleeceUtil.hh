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
    
    // SQLite value subtypes for extended Fleece type information
    static const int kFleeceIntBoolean      = 0x68;   // Result is boolean type (expressed as int)
    static const int kFleeceIntUnsigned     = 0x69;   // Result is an unsigned integer


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
    const fleece::Value* evaluatePath(sqlite3_context *ctx,
                                      slice path,
                                      const fleece::Value *val) noexcept;
    bool evaluatePath(sqlite3_context *ctx, sqlite3_value **argv, const fleece::Value* *outValue);

    void setResultFromValue(sqlite3_context*, const fleece::Value*) noexcept;
    void setResultFromValueType(sqlite3_context*, const fleece::Value*) noexcept;
    void setResultTextFromSlice(sqlite3_context*, slice) noexcept;
    void setResultBlobFromSlice(sqlite3_context*, slice) noexcept;
    bool setResultBlobFromEncodedValue(sqlite3_context*, const fleece::Value*);

    //// Registering SQLite functions:

    struct SQLiteFunctionSpec {
        const char *name;
        int argCount;
        void (*function)(sqlite3_context*,int,sqlite3_value**);
        void (*stepCallback)(sqlite3_context*,int,sqlite3_value**);
        void (*finalCallback)(sqlite3_context*);
    };

    extern const SQLiteFunctionSpec kFleeceFunctionsSpec[];
    extern const SQLiteFunctionSpec kRankFunctionsSpec[];
    extern const SQLiteFunctionSpec kN1QLFunctionsSpec[];

    int RegisterFleeceEachFunctions(sqlite3 *db, DataFile::FleeceAccessor,
                                    fleece::SharedKeys*);

}
