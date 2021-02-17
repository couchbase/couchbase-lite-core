//
// TestsCommon.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/slice.hh"
#include "JSON5.hh"
#include "function_ref.hh"
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#ifdef CATCH_VERSION_MAJOR
#error "This header must be included before Catch.hpp"
#endif


namespace litecore {
    class FilePath;
}

/** Returns the OS's temporary directory (/tmp on Unix-like systems) */
litecore::FilePath GetSystemTempDirectory();

/** Returns a temporary directory for use by this test run. */
litecore::FilePath GetTempDirectory();

/** Initializes logging for tests, both binary and console. */
void InitTestLogging();


std::string sliceToHex(fleece::slice);
std::string sliceToHexDump(fleece::slice, size_t width = 16);

// Converts a C4Slice or C4SliceResult to a C++ string.
static inline std::string toString(fleece::slice s)   {return std::string(s);}
static inline std::string toString(FLSlice s)         {return std::string(fleece::slice(s));}
static inline std::string toString(const FLSliceResult &s)   {return std::string((char*)s.buf, s.size);}
static inline std::string toString(FLSliceResult &&s) {return std::string(fleece::alloc_slice(std::move(s)));}


// Converts JSON5 to JSON; helps make JSON test input more readable!
std::string json5(std::string_view);
fleece::alloc_slice json5slice(std::string_view);


#pragma mark - STREAM OPERATORS FOR LOGGING:


// These '<<' functions help Catch log values of custom types in assertion messages.
// WARNING: For obscure C++ reasons these must be declared _before_ including Catch.hh
//          or they won't work inside Catch macros like CHECK().


// These operators write a slice for debugging. The value is wrapped with `slice[...]`,
// and if its contents aren't ASCII they are written in hex form instead.
namespace fleece {
    std::ostream& operator<< (std::ostream& o, pure_slice s);
}
static inline std::ostream& operator<< (std::ostream& o, FLSlice s) {return o << fleece::slice(s);}
static inline std::ostream& operator<< (std::ostream& o, FLSliceResult s) {return o << fleece::slice(s);}


// Logging std::set instances to cerr or Catch.
// This lets you test functions returning sets in CHECK or REQUIRE.
template <class T>
std::ostream& operator<< (std::ostream &o, const std::set<T> &things) {
    o << "{";
    int n = 0;
    for (const T &thing : things) {
        if (n++) o << ", ";
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


// Waits for the predicate to return true, blocking the current thread and checking every 100ms.
// If the timeout (given in **milliseconds**) elapses, calls FAIL.
void WaitUntil(int timeoutMillis, fleece::function_ref<bool()> predicate);

