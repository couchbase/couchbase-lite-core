//
// TestsCommon.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include "fleece/function_ref.hh"
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#ifdef CATCH_VERSION_MAJOR
#    error "This header must be included before Catch.hpp"
#endif


// Enables use of suffixed numeric literals, like `500ms` or `2s`, for std::chrono::time_interval,
// as well as `"..."s` for std::string literals.
using namespace std::literals;

namespace litecore {
    class FilePath;
}

/** Returns the OS's temporary directory (/tmp on Unix-like systems) */
litecore::FilePath GetSystemTempDirectory();

/** Returns a temporary directory for use by this test run. */
litecore::FilePath GetTempDirectory();

/** Reads a text file, passing each line to the callback function. */
bool ReadFileByLines(const std::string& path, fleece::function_ref<bool(FLSlice)> callback, size_t maxLines);

/** Initializes logging for tests, both binary and console. */
void InitTestLogging();


std::string sliceToHex(fleece::pure_slice);
std::string sliceToHexDump(fleece::pure_slice, size_t width = 16);

// Converts a C4Slice or C4SliceResult to a C++ string.
inline std::string toString(fleece::slice s) { return std::string(s); }

inline std::string toString(FLSlice s) { return std::string(fleece::slice(s)); }

inline std::string toString(const FLSliceResult& s) { return {(char*)s.buf, s.size}; }

inline std::string toString(FLSliceResult&& s) { return std::string(fleece::alloc_slice(s)); }

// Converts JSON5 to JSON; helps make JSON test input more readable!
std::string         json5(std::string_view);
fleece::alloc_slice json5slice(std::string_view);


#pragma mark - STREAM OPERATORS FOR LOGGING:

// These '<<' functions help Catch log values of custom types in assertion messages.
// WARNING: For obscure C++ reasons these must be declared _before_ including Catch.hh
//          or they won't work inside Catch macros like CHECK().


// These operators write a slice for debugging. The value is wrapped with `slice[...]`,
// and if its contents aren't ASCII they are written in hex form instead.
namespace fleece {
    std::ostream& operator<<(std::ostream& o, pure_slice s);
}

inline std::ostream& operator<<(std::ostream& o, FLSlice s) { return o << fleece::slice(s); }

inline std::ostream& operator<<(std::ostream& o, FLSliceResult s) { return o << fleece::slice(s); }

// Logging std::set instances to cerr or Catch.
// This lets you test functions returning sets in CHECK or REQUIRE.
template <class T>
std::ostream& operator<<(std::ostream& o, const std::set<T>& things) {
    o << "{";
    int n = 0;
    for ( const T& thing : things ) {
        if ( n++ ) o << ", ";
        o << '"' << thing << '"';
    }
    o << "}";
    return o;
}

#pragma mark - SUPPRESSING EXCEPTION WARNINGS:

// RAII utility to suppress reporting C++ exceptions (or breaking at them, in the Xcode debugger.)
// Declare an instance when testing something that's expected to throw an exception internally.
struct ExpectingExceptions {
    ExpectingExceptions();
    ~ExpectingExceptions();
};

#pragma mark - MISC.:


// Waits for the predicate to return true, blocking the current thread and checking every 50ms
// until the timeout expires.
// (You can express the timeout as a decimal literal followed by `ms`, e.g. `500ms`.)
// The predicate is guaranteed to be checked at least once, immediately when WaitUntil is called.
// Returns true if the predicate became true; or false if the timeout expired.
[[nodiscard]] bool WaitUntil(std::chrono::milliseconds, fleece::function_ref<bool()> predicate);


// CHECK that CONDITION will become true before TIMEOUT expires.
// (You can express the timeout as a decimal literal followed by `s` or `ms`, e.g. `2s`.)
#define CHECK_BEFORE(TIMEOUT, CONDITION)                                                                               \
    do {                                                                                                               \
        auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(TIMEOUT);                                     \
        if ( !WaitUntil(_ms, [&] { return (CONDITION); }) )                                                            \
            FAIL_CHECK(#CONDITION << " did not occur within " << _ms.count() << "ms");                                 \
    } while ( false )

// REQUIRE that CONDITION will become true before TIMEOUT expires.
// (You can express the timeout as a decimal literal followed by `s` or `ms`, e.g. `2s`.)
#define REQUIRE_BEFORE(TIMEOUT, CONDITION)                                                                             \
    do {                                                                                                               \
        auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(TIMEOUT);                                     \
        if ( !WaitUntil(_ms, [&] { return (CONDITION); }) )                                                            \
            FAIL(#CONDITION << " did not occur within " << _ms.count() << "ms");                                       \
    } while ( false )
