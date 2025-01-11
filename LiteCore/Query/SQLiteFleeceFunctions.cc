//
// SQLiteFleeceFunctions.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Base64.hh"
#include "Error.hh"
#include "Encoder.hh"
#include "Logging.hh"
#include "DeepIterator.hh"
#include "NumConversion.hh"
#include "RevID.hh"
#include <sstream>

using namespace fleece;
using namespace fleece::impl;
using namespace std;

namespace litecore {

    // Core SQLite functions for accessing values inside Fleece blobs.
    //
    // In the API descriptions below, `body` refers to the value of the `body` column of the SQLite row.
    // `propertyPath` refers to a string giving a path to a property, as used by the Fleece `KeyPath` class.


    /// fl_root(body) -> fleeceData
    ///
    /// Gets the root of the current document given the SQLite row's `body` column.
    static void fl_root(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        if ( sqlite3_value_type(argv[0]) == SQLITE_BLOB ) {
            // Pull the Fleece data out of a raw document body:
            bool  copied{false};
            slice body = valueAsDocBody(argv[0], copied);
            setResultBlobFromFleeceData(ctx, body);
            if ( copied ) ::free((void*)body.buf);
        } else {
            // If arg isn't a blob, check if it's a tagged Fleece pointer:
            const Value* val = asFleeceValue(argv[0]);
            if ( val ) {
                sqlite3_result_pointer(ctx, (void*)val, kFleeceValuePointerType, nullptr);
            } else {
                DebugAssert(sqlite3_value_type(argv[0]) == SQLITE_NULL);
                sqlite3_result_null(ctx);
            }
        }
    }

    /// fl_value(body, propertyPath) -> propertyValue
    ///
    /// The most common property accessor. Gets a property of the document given its path.
    __hot static void fl_value(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            setResultFromValue(ctx, scope.root);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_value: exception!", -1); }
    }

    /// fl_version(version) -> string
    ///
    /// Decodes a binary revid (the SQLite row's `version` column) to a string.
    static void fl_version(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            slice version = valueAsSlice(argv[0]);
            setResultTextFromSlice(ctx, revid(version).expanded());
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_version: exception!", -1); }
    }

    /// fl_blob(body, propertyPath) -> blob data
    ///
    /// Fetches a blob from a document, given the `body` column and the path to the blob metadata dict.
    static void fl_blob(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if ( !scope.root ) return;
            const Dict* blobDict = scope.root->asDict();
            if ( !blobDict ) return;
            // Read the blob data:
            auto delegate = getDBDelegate(ctx);
            if ( !delegate ) return;
            alloc_slice data;
            try {
                data = delegate->blobAccessor(blobDict);
            } catch ( const std::exception& ) {
                // ignore exception; just return 'missing'
            }
            setResultBlobFromData(ctx, data);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "unexpected error reading blob", -1); }
    }

    /// fl_nested_value(fleeceData, propertyPath) -> propertyValue
    ///
    /// This function is used by the JSON `"._"` operator to extract a nested property out of a runtime value.
    static void fl_nested_value(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceParam val{ctx, argv[0], false};
            if ( !val ) {
                sqlite3_result_null(ctx);
                return;
            }
            setResultFromValue(ctx, evaluatePathFromArg(ctx, argv, 1, val));
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_nested_value: exception!", -1); }
    }

    // (subroutine of `fl_fts_value`. Writes a Fleece scalar value to a stream in text form.
    static void handle_fts_value(const Value* data, stringstream& result) {
        if ( _usuallyFalse(!data) ) {
            Warn("Null value received in handle_fts_value");
            return;
        }

        switch ( data->type() ) {
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

    /// fl_fts_value(body, propertyPath) -> text data
    ///
    /// Converts a document property to a string, for indexing by FTS.
    /// Recurses into arrays and dicts, concatenating all the scalars found therein.
    static void fl_fts_value(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if ( scope.root == nullptr ) { return; }

            stringstream result;
            for ( DeepIterator j(scope.root); j; ++j ) {
                handle_fts_value(j.value(), result);
                result << " ";
            }

            const string resultStr = result.str();
            setResultTextFromSlice(ctx, slice(resultStr.substr(0, resultStr.size() - 1)));
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_fts_value: exception!", -1); }
    }

    /// fl_unnested_value(unnestTableBody [, propertyPath]) -> propertyValue
    ///
    /// The equivalent of `fl_root` and `fl_value` for UNNEST indexes, i.e. tables containing unnested properties.
    static void fl_unnested_value(sqlite3_context* ctx, int argc, sqlite3_value** argv) noexcept {
        DebugAssert(argc == 1 || argc == 2);
        sqlite3_value* body = argv[0];
        if ( sqlite3_value_type(body) == SQLITE_BLOB ) {
            // body is Fleece data:
            if ( argc == 1 ) return fl_root(ctx, argc, argv);
            else
                return fl_value(ctx, argc, argv);
        } else {
            // body is a SQLite value; just return it
            if ( argc == 1 ) sqlite3_result_value(ctx, body);
            else
                sqlite3_result_null(ctx);
        }
    }

    /// fl_exists(body, propertyPath) -> 0/1
    ///
    /// Returns true if the given property path exists in the document.
    static void fl_exists(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            sqlite3_result_int(ctx, (scope.root ? 1 : 0));
            sqlite3_result_subtype(ctx, kFleeceIntBoolean);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_exists: exception!", -1); }
    }

    /// fl_count(body, propertyPath) -> int
    ///
    /// Implements the N1QL `COUNT` function on a property of a document.
    static void fl_count(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            if ( !scope.root ) {
                sqlite3_result_null(ctx);
                return;
            }
            switch ( scope.root->type() ) {
                case kArray:
                    {
                        ArrayIterator a(scope.root->asArray());
                        int64_t       count = 0;
                        while ( a ) {
                            if ( a.value()->type() != kNull ) { count++; }

                            ++a;
                        }

                        sqlite3_result_int64(ctx, count);
                        break;
                    }
                case kDict:
                    sqlite3_result_int(ctx, narrow_cast<int>(scope.root->asDict()->count()));
                    break;
                default:
                    sqlite3_result_null(ctx);
                    break;
            }
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_count: exception!", -1); }
    }

    /// fl_result(value) -> value suitable for use as a result column
    ///
    /// This function is wrapped around SQLite result (projection) expressions, to ensure they're returned from the
    /// SQLite statement in a form that's readable by `SQLiteQueryRunner::encodeColumn()`.
    /// It converts the various custom `sqlite3_value` subtypes into Fleece containers.
    __hot static void fl_result(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            auto arg = argv[0];
            switch ( sqlite3_value_type(arg) ) {
                case SQLITE_NULL:
                    {
                        // A SQLite encoded pointer looks like a NULL:
                        const Value* value = asFleeceValue(arg);
                        if ( value ) {
                            setResultBlobFromEncodedValue(ctx, value);
                            return;
                        }
                        break;
                    }
                case SQLITE_INTEGER:
                    {
                        auto subtype = sqlite3_value_subtype(arg);
                        if ( subtype == kFleeceIntBoolean ) {
                            // A tagged boolean:
                            slice encoded =
                                    sqlite3_value_int(arg) ? Encoder::kPreEncodedTrue : Encoder::kPreEncodedFalse;
                            sqlite3_result_blob(ctx, encoded.buf, int(encoded.size), SQLITE_STATIC);
                            return;
                        } else if ( subtype == kFleeceIntUnsigned ) {
                            Encoder enc;
                            enc.writeUInt(sqlite3_value_int64(arg));
                            setResultBlobFromFleeceData(ctx, enc.finish());
                            return;
                        }
                        break;
                    }
                case SQLITE_BLOB:
                    {
                        switch ( sqlite3_value_subtype(arg) ) {
                            case 0:
                                // Untagged blob is already Fleece data
                                break;
                            case kFleeceNullSubtype:
                                {
                                    // A tagged Fleece/JSON null:
                                    slice encoded = Encoder::kPreEncodedNull;
                                    sqlite3_result_blob(ctx, encoded.buf, int(encoded.size), SQLITE_STATIC);
                                    return;
                                }
                            case kPlainBlobSubtype:
                                {
                                    // A plain blob/data value has to be wrapped in a Fleece container to avoid
                                    // misinterpretation, since SQLiteQueryRunner will assume all blob results
                                    // are Fleece containers.
                                    Encoder enc;
                                    enc.writeData(valueAsSlice(arg));
                                    setResultBlobFromFleeceData(ctx, enc.finish());
                                    return;
                                }
                            default:
                                Assert(false, "Invalid blob subtype");
                        }
                        break;
                    }
            }

            // Default behavior if none of the special cases above apply: just return directly
            sqlite3_result_value(ctx, arg);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_result: exception!", -1); }
    }

    /// fl_null() -> value
    ///
    /// Simply creates & returns a value of Fleece/N1QL `null` type (not SQLite NULL, which is unknown/MISSING).
    static void fl_null(sqlite3_context* ctx, [[maybe_unused]] int argc,
                        [[maybe_unused]] sqlite3_value** argv) noexcept {
        sqlite3_result_zeroblob(ctx, 0);
        sqlite3_result_subtype(ctx, kFleeceNullSubtype);
    }

    /// fl_bool(i) -> true/false
    ///
    /// Creates a Fleece boolean value from a SQLite value. Anything that coerces to a non-zero int is `true`.
    static void fl_bool(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        sqlite3_result_int(ctx, sqlite3_value_int(argv[0]) != 0);
        sqlite3_result_subtype(ctx, kFleeceIntBoolean);
    }

    /// fl_boolean_result(value) -> value suitable for use as a result column
    ///
    /// Used for functions that SQLite returns as integers, that actually need a true or false
    static void fl_boolean_result(sqlite3_context* ctx, int argc, sqlite3_value** argv) noexcept {
        enhanced_bool_t result = booleanValue(ctx, argv[0]);
        if ( result == kTrue || result == kFalse ) {
            slice encoded = result ? Encoder::kPreEncodedTrue : Encoder::kPreEncodedFalse;
            sqlite3_result_blob(ctx, encoded.buf, int(encoded.size), SQLITE_STATIC);
        } else {
            fl_result(ctx, argc, argv);
        }
    }

#pragma mark - CONTAINS()

    /// fl_contains(body, propertyPath, value) -> 0/1
    ///
    static void fl_contains(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        try {
            QueryFleeceScope scope(ctx, argv);
            collectionContainsImpl(ctx, scope.root, argv[2]);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_contains: exception!", -1); }
    }

    void collectionContainsImpl(sqlite3_context* ctx, const Value* collection, sqlite3_value* arg) {
        if ( !collection || collection->type() < kArray ) {
            sqlite3_result_zeroblob(ctx, 0);  // JSON null
            return;
        }

        // Set up a predicate callback that will match the desired value:
        union target_t {
            int64_t i;
            double  d;
            FLSlice s;
        };

        target_t  target;  // NOLINT(cppcoreguidelines-pro-type-member-init)
        valueType targetType;
        bool (*predicate)(const Value*, const target_t&);

        switch ( sqlite3_value_type(arg) ) {
            case SQLITE_INTEGER:
                {
                    targetType = kNumber;
                    target.i   = sqlite3_value_int64(arg);
                    predicate  = [](const Value* v, const target_t& t) { return v->asInt() == t.i; };
                    break;
                }
            case SQLITE_FLOAT:
                {
                    targetType = kNumber;
                    target.d   = sqlite3_value_double(arg);
                    predicate  = [](const Value* v, const target_t& t) { return v->asDouble() == t.d; };
                    break;
                }
            case SQLITE_TEXT:
                {
                    targetType = kString;
                    target.s   = slice(sqlite3_value_blob(arg), sqlite3_value_bytes(arg));
                    predicate  = [](const Value* v, const target_t& t) { return v->asString() == slice(t.s); };
                    break;
                }
            case SQLITE_BLOB:
                {
                    if ( sqlite3_value_bytes(arg) == 0 ) {
                        // A zero-length blob represents a Fleece/JSON 'null'.
                        sqlite3_result_zeroblob(ctx, 0);  // JSON null
                        return;
                    } else {
                        targetType = kData;
                        target.s   = slice(sqlite3_value_blob(arg), sqlite3_value_bytes(arg));
                        predicate  = [](const Value* v, const target_t& t) { return v->asData() == slice(t.s); };
                    }
                    break;
                }
            case SQLITE_NULL:
            default:
                {
                    // A SQL null (MISSING) doesn't match anything
                    sqlite3_result_null(ctx);
                    return;
                }
        }

        // predicate tests the first argument, which will come from the array, against the second argument,
        // the target of the query whether it is contained in the array. Before applying it, they must be
        // of compatible types. We require they have identical valueTypes except for kBoolean, in which case they
        // are to be compared as integer. Whether the target is a kNumber or kBoolean, its type appears here as kNumber.
        // We only need to convert the type of the array element.
        auto comparable = [](valueType vtype1, valueType targetTyp) {
            // pre-condition: targetType != kBoolean
            return vtype1 == targetTyp ? true : (targetTyp == kNumber ? vtype1 == kBoolean : false);
        };

        // Now iterate the array/dict:
        bool found = false;
        if ( collection->type() == kArray ) {
            for ( Array::iterator j(collection->asArray()); j; ++j ) {
                auto val = j.value();
                if ( comparable(val->type(), targetType) && predicate(val, target) ) {
                    found = true;
                    break;
                }
            }
        } else {
            for ( Dict::iterator j(collection->asDict()); j; ++j ) {
                auto val = j.value();
                if ( comparable(val->type(), targetType) && predicate(val, target) ) {
                    found = true;
                    break;
                }
            }
        }
        sqlite3_result_int(ctx, found);
        sqlite3_result_subtype(ctx, kFleeceIntBoolean);
    }

#pragma mark - ARRAY() and OBJECT()

    // writes a SQLite arg value to a Fleece Encoder. Returns false on failure
    static bool writeSQLiteValue(sqlite3_context* ctx, sqlite3_value* arg, slice key, Encoder& enc) {
        auto type = sqlite3_value_type(arg);
        if ( key && type != SQLITE_NULL ) enc.writeKey(key);
        switch ( type ) {
            case SQLITE_INTEGER:
                {
                    auto intVal = sqlite3_value_int64(arg);
                    if ( sqlite3_value_subtype(arg) == kFleeceIntBoolean ) enc.writeBool(intVal != 0);
                    else
                        enc.writeInt(intVal);
                    break;
                }
            case SQLITE_FLOAT:
                enc.writeDouble(sqlite3_value_double(arg));
                break;
            case SQLITE_TEXT:
                enc.writeString(valueAsStringSlice(arg));
                break;
            case SQLITE_BLOB:
                {
                    switch ( sqlite3_value_subtype(arg) ) {
                        case 0:
                            {
                                const QueryFleeceParam value{ctx, arg};
                                if ( !value ) return false;  // error occurred
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
            case SQLITE_NULL:
            default:
                {
                    const Value* value = asFleeceValue(arg);
                    if ( value ) {
                        if ( key ) enc.writeKey(key);
                        enc.writeValue(value);
                    }
                    break;
                }
        }
        return true;
    }

    /// array_of(value...) -> Array
    ///
    /// The N1QL `ARRAY()` function. Constructs a Fleece Array out of its arguments.
    static void array_of(sqlite3_context* ctx, int argc, sqlite3_value** argv) noexcept {
        Encoder enc;
        enc.beginArray(argc);
        for ( int i = 0; i < argc; i++ ) {
            if ( !writeSQLiteValue(ctx, argv[i], nullslice, enc) ) return;
        }
        enc.endArray();
        setResultBlobFromFleeceData(ctx, enc.finish());
    }

    /// dict_of(key, value, ...) -> Dict
    ///
    /// The N1QL `OBJECT()` function. Constructs a Fleece Dict out of its arguments, which are alternating keys & values.
    static void dict_of(sqlite3_context* ctx, int argc, sqlite3_value** argv) noexcept {
        if ( argc % 2 ) {
            sqlite3_result_error(ctx, "object() must have an even arg count", -1);
            return;
        }
        Encoder enc;
        enc.beginDictionary(argc / 2);
        for ( int i = 0; i < argc; i += 2 ) {
            slice key = valueAsStringSlice(argv[i]);
            if ( !key ) {
                sqlite3_result_error(ctx, "invalid key arg to object()", -1);
                return;
            }
            if ( !writeSQLiteValue(ctx, argv[i + 1], key, enc) ) return;
        }
        enc.endDictionary();
        setResultBlobFromFleeceData(ctx, enc.finish());
    }

#pragma mark - REVISION HISTORY:

    /// fl_callback(docID, revID, body, extra, sequence, callback, flags) -> string
    ///
    /// Invokes a C callback function (given as a SQLite query parameter) and passes it all the columns of the
    /// current document.
    /// Not used in N1QL queries! This is an optimization invoked by the `SQLiteKeyStore::withDocBodies()` method
    /// so it can read documents as quickly as possible.
    static void fl_callback(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        RecordUpdate rec(valueAsSlice(argv[0]), valueAsSlice(argv[2]));
        rec.version   = valueAsSlice(argv[1]);
        rec.extra     = valueAsSlice(argv[3]);
        rec.sequence  = sequence_t(sqlite3_value_int(argv[4]));
        rec.flags     = (DocumentFlags)sqlite3_value_int(argv[5]);
        auto callback = sqlite3_value_pointer(argv[6], kWithDocBodiesCallbackPointerType);
        if ( !callback || !rec.key ) {
            sqlite3_result_error(ctx, "Missing or invalid callback", -1);
            return;
        }
        try {
            alloc_slice result = (*(KeyStore::WithDocBodyCallback*)callback)(rec);
            setResultTextFromSlice(ctx, result);
        } catch ( const std::exception& ) { sqlite3_result_error(ctx, "fl_callback: exception!", -1); }
    }

#pragma mark - VECTOR (ML) SEARCH:

    // Subroutine that creates a SQLite blob from raw vector data.
    static const char* encodeVectorFromBytes(sqlite3_context* ctx, slice data, int dim = 0) {
        if ( data.size < 2 * sizeof(float) || data.size % sizeof(float) != 0 ) {
            return "data is wrong length to be a vector";
        } else if ( dim > 0 && data.size / sizeof(float) != dim ) {
            return "vector has wrong number of dimensions";
        } else {
            setResultBlobFromData(ctx, data, kPlainBlobSubtype);
            return nullptr;
        }
    }

    // Subroutine that converts a Fleece Value to a raw vector and puts it in the SQLite result.
    // On error it returns the message as a string; on success, nullptr.
    static const char* encodeVector(sqlite3_context* ctx, const fleece::impl::Value* value, int dim = 0) {
        switch ( value->type() ) {
            case kArray:
                {
                    auto   array = value->asArray();
                    size_t n     = array->count();
                    if ( n < 2 || (dim > 0 && n != dim) ) return "vector has wrong number of dimensions";
                    vector<float> vec(n);
                    size_t        i = 0;
                    for ( ArrayIterator iter(array); iter; ++iter ) {
                        if ( auto item = iter.value(); item->type() == kNumber ) {
                            vec[i++] = item->asFloat();
                        } else {
                            return "array contains a non-numeric value";
                        }
                    }
                    setResultBlobFromData(ctx, slice{vec.data(), vec.size() * sizeof(float)}, kPlainBlobSubtype);
                    return nullptr;
                }
            case kData:
                return encodeVectorFromBytes(ctx, value->asData(), dim);
            case kString:
                return encodeVectorFromBytes(ctx, base64::decode(value->asString()), dim);
            default:
                return "value is wrong type to be a vector";
        }
    }

    // Subroutine that converts a SQLite argument to a raw vector and puts it in the SQLite result.
    // On error it returns the message as a string; on success, nullptr.
    static const char* encodeVector(sqlite3_context* ctx, sqlite3_value* arg, int dim = 0) {
        if ( const QueryFleeceParam flVal{ctx, arg, false}; flVal != nullptr ) {
            // Arg is a wrapped Fleece value:
            return encodeVector(ctx, flVal, dim);
        } else if ( sqlite3_value_type(arg) == SQLITE_BLOB && sqlite3_value_subtype(arg) == kPlainBlobSubtype ) {
            // Arg is a N1QL blob, probably retrieved from a document blob reference:
            if ( auto len = sqlite3_value_bytes(arg); len > 0 && len % sizeof(float) == 0 ) {
                if ( dim > 0 && len / sizeof(float) != dim ) return "vector has wrong number of dimensions";
                // Raw blob, multiple of 4 bytes long:
                sqlite3_result_subtype(ctx, kPlainBlobSubtype);
                sqlite3_result_value(ctx, arg);
                return nullptr;
            } else {
                return "raw vector data length not multiple of 4";
            }
        } else if ( sqlite3_value_type(arg) == SQLITE_TEXT ) {
            // Arg is a Base64 string:
            return encodeVectorFromBytes(ctx, base64::decode(valueAsStringSlice(arg)), dim);
        } else {
            return "value is wrong type to be a vector";
        }
    }

    /// fl_vector_to_index(body, propertyPath, dimensions) -> blob (array of float32) or NULL
    /// fl_vector_to_index(expr, NULL,         dimensions) -> blob (array of float32) or NULL
    ///
    /// Gets a vector value -- from a document property or given directly -- and converts it into a blob of float32s.
    /// @note This is used when indexing docs for a vector index, so invalid vector data will produce
    ///       a SQLite NULL result instead of an error.
    static void fl_vector_to_index(sqlite3_context* ctx, int argc, sqlite3_value** argv) noexcept {
        int dim = sqlite3_value_int(argv[argc - 1]);
        if ( dim <= 0 ) {
            // Invalid number of dimensions is an internal error, so it should fail.
            sqlite3_result_error(ctx, "Invalid number of dimensions in fl_vector_to_index", -1);
            return;
        }
        const char* errorMsg = nullptr;
        switch ( sqlite3_value_type(argv[1]) ) {
            case SQLITE_TEXT:
                // 1st arg is doc body, 2nd arg is a property path:
                if ( QueryFleeceScope scope(ctx, argv); scope.root ) {
                    errorMsg = encodeVector(ctx, scope.root, dim);
                    if ( errorMsg )
                        Warn("Updating vector index: Property '%s' %s; ignoring", sqlite3_value_text(argv[1]),
                             errorMsg);
                } else {
                    sqlite3_result_null(ctx);  // missing property
                }
                break;
            case SQLITE_NULL:
                // 1st arg is the vector itself:
                errorMsg = encodeVector(ctx, argv[0], dim);
                if ( errorMsg && sqlite3_value_type(argv[0]) != SQLITE_NULL )
                    Warn("Updating vector index: %s; ignoring", errorMsg);
                break;
            default:
                sqlite3_result_error(ctx, "Invalid 2nd arg to fl_vector_to_index", -1);
        }

        if ( errorMsg ) sqlite3_result_null(ctx);
    }

    /// encode_vector(fleece_array or raw blob) -> blob data (array of float32)
    ///
    /// Converts a vector value to its raw form, a blob of packed float32s.
    /// Unlike `fl_vector_to_index` this does return an error given invalid input.
    static void encode_vector(sqlite3_context* ctx, [[maybe_unused]] int argc, sqlite3_value** argv) noexcept {
        const char* errorMsg = encodeVector(ctx, argv[0]);
        if ( errorMsg ) sqlite3_result_error(ctx, errorMsg, -1);
    }

#pragma mark - REGISTRATION:


    const SQLiteFunctionSpec kFleeceFunctionsSpec[] = {{"fl_root", 1, fl_root},
                                                       {"fl_value", 2, fl_value},
                                                       {"fl_version", 1, fl_version},
                                                       {"fl_nested_value", 2, fl_nested_value},
                                                       {"fl_fts_value", 2, fl_fts_value},
                                                       {"fl_blob", 2, fl_blob},
                                                       {"fl_exists", 2, fl_exists},
                                                       {"fl_count", 2, fl_count},
                                                       {"fl_contains", 3, fl_contains},
                                                       {"fl_result", 1, fl_result},
                                                       {"fl_boolean_result", 1, fl_boolean_result},
                                                       {"fl_null", 0, fl_null},
                                                       {"fl_bool", 1, fl_bool},
                                                       {"array_of", -1, array_of},
                                                       {"dict_of", -1, dict_of},
                                                       {"fl_callback", 7, fl_callback},
                                                       {"fl_vector_to_index", 3, fl_vector_to_index},
                                                       {"encode_vector", 1, encode_vector},
                                                       {}};

    const SQLiteFunctionSpec kFleeceNullAccessorFunctionsSpec[] = {{"fl_unnested_value", -1, fl_unnested_value}, {}};
}  // namespace litecore
