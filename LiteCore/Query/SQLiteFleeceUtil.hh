//
// SQLiteFleeceUtil.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "DataFile.hh"
#include "SQLite_Internal.hh"
#include "FleeceImpl.hh"
#include <sqlite3.h>

namespace litecore {
    class CollationContext;

    // SQLite value subtypes to represent type info that SQL doesn't convey:
    enum {
        kPlainBlobSubtype = 0x66,  // Blob is raw data (otherwise assumed to be Fleece)
        kFleeceNullSubtype,        // Zero-length blob representing JSON null
        kFleeceIntBoolean,         // Integer is a boolean (true or false)
        kFleeceIntUnsigned,        // Integer is unsigned
    };

    enum enhanced_bool_t : uint8_t { kFalse, kTrue, kMissing, kJSONNull };

    extern const char *const kFleeceValuePointerType;

    // Given an arg whose value is either a doc's `body` column or raw Fleece, this returns the
    // Fleece data.
    // If it detects a v2.x-format `body` it extracts the current revision's Fleece out of the
    // serialized rev-tree. That data might be at an odd address; if so it copies it to a new
    // heap block and sets `outCopied` to true. As a result, if `outCopied` returns set to true,
    // you MUST call `::free((void*)data.buf` on the result.
    slice valueAsDocBody(sqlite3_value *arg, bool &outCopied);

    static inline const fleece::impl::Value *asFleeceValue(sqlite3_value *value) {
        return (const fleece::impl::Value *)sqlite3_value_pointer(value, kFleeceValuePointerType);
    }

    // Takes a document body from argv[0] and key-path from argv[1].
    // Establishes a scope for the Fleece data, and evaluates the path, setting `root`
    class QueryFleeceScope : public fleece::impl::Scope {
      public:
        QueryFleeceScope(sqlite3_context *ctx, sqlite3_value **argv);
        ~QueryFleeceScope();

        const fleece::impl::Value *root;

      private:
        bool _copied;  // Do not initialize it; the base class will do.
    };

    static inline DataFile::Delegate *getDBDelegate(sqlite3_context *ctx) {
        return ((fleeceFuncContext *)sqlite3_user_data(ctx))->delegate;
    }

    // Returns the data of a SQLite blob value as a slice
    static inline slice valueAsSlice(sqlite3_value *arg) noexcept {
        const void *blob = sqlite3_value_blob(arg);  // must be called _before_ sqlite3_value_bytes
        return slice(blob, sqlite3_value_bytes(arg));
    }

    // Returns the data of a SQLite string value as a slice
    static inline slice valueAsStringSlice(sqlite3_value *arg) noexcept {
        auto blob = sqlite3_value_text(arg);  // must be called _before_ sqlite3_value_bytes
        return slice(blob, sqlite3_value_bytes(arg));
    }

    // Interprets the arg, which must be a blob, as a Fleece value and returns it as a Value*.
    // On error returns nullptr (and sets the SQLite result error.)
    const fleece::impl::Value *fleeceParam(sqlite3_context *, sqlite3_value *arg, bool required = true) noexcept;

    // Evaluates a path from the current value of *pValue and stores the result back to
    // *pValue. Returns a SQLite error code, or SQLITE_OK on success.
    int evaluatePath(slice path, const fleece::impl::Value **pValue) noexcept;

    const fleece::impl::Value *evaluatePathFromArg(sqlite3_context *, sqlite3_value **argv, int argNo,
                                                   const fleece::impl::Value *root);

    // Sets the function result based on a Value*
    void setResultFromValue(sqlite3_context *, const fleece::impl::Value *) noexcept;

    // Sets the function result to a string, from the given slice.
    // If the slice is null, sets the function result to SQLite null.
    void setResultTextFromSlice(sqlite3_context *, slice) noexcept;
    void setResultTextFromSlice(sqlite3_context *, alloc_slice) noexcept;

    // Sets the function result to a blob, with optional subtype
    void setResultBlobFromData(sqlite3_context *, slice, int subtype = kPlainBlobSubtype) noexcept;
    void setResultBlobFromData(sqlite3_context *, alloc_slice, int subtype = kPlainBlobSubtype) noexcept;

    // Sets the function result to a Fleece container (a blob with subtype 0)
    static inline void setResultBlobFromFleeceData(sqlite3_context *ctx, slice blob) noexcept {
        setResultBlobFromData(ctx, blob, 0);
    }

    static inline void setResultBlobFromFleeceData(sqlite3_context *ctx, alloc_slice blob) noexcept {
        setResultBlobFromData(ctx, blob, 0);
    }

    // Encodes the Value as a Fleece container and sets it as the result
    bool setResultBlobFromEncodedValue(sqlite3_context *, const fleece::impl::Value *) noexcept;

    // Sets the function result to be a Fleece/JSON null (an empty blob with kFleeceNullSubtype)
    void setResultFleeceNull(sqlite3_context *) noexcept;

    // Common implementation of fl_contains and array_contains
    void collectionContainsImpl(sqlite3_context *, const fleece::impl::Value *collection, sqlite3_value *arg);

    // Given an argument containing the name of a collation, returns a CollationContext.
    // If the argument doesn't exist, returns a default context (case-sensitive, Unicode-aware.)
    CollationContext &collationContextFromArg(sqlite3_context *ctx, int argc, sqlite3_value **argv, int argNo);

    enhanced_bool_t booleanValue(sqlite3_context *ctx, sqlite3_value *arg);

    //// Registering SQLite functions:

    struct SQLiteFunctionSpec {
        const char *name;
        int         argCount;
        void (*function)(sqlite3_context *, int, sqlite3_value **);
        void (*stepCallback)(sqlite3_context *, int, sqlite3_value **);
        void (*finalCallback)(sqlite3_context *);
    };

    extern const SQLiteFunctionSpec kFleeceFunctionsSpec[];
    extern const SQLiteFunctionSpec kFleeceNullAccessorFunctionsSpec[];
    extern const SQLiteFunctionSpec kRankFunctionsSpec[];
    extern const SQLiteFunctionSpec kN1QLFunctionsSpec[];
#ifdef COUCHBASE_ENTERPRISE
    extern const SQLiteFunctionSpec kPredictFunctionsSpec[];
#endif

    int RegisterFleeceEachFunctions(sqlite3 *db, const fleeceFuncContext &);

}  // namespace litecore
