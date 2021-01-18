//
// TestsCommon.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/slice.hh"
#include "JSON5.hh"
#include <iostream>
#include <string_view>

namespace litecore {
    class FilePath;
}

/** Returns the OS's temporary directory (/tmp on Unix-like systems) */
litecore::FilePath GetSystemTempDirectory();

/** Returns a temporary directory for use by this test run. */
litecore::FilePath GetTempDirectory();

/** Initializes logging for tests, both binary and console. */
void InitTestLogging();



// These '<<' functions help Catch log values of custom types in assertion messages:
namespace fleece {
    std::ostream& operator<< (std::ostream& o, pure_slice s);
}
static inline std::ostream& operator<< (std::ostream& o, FLSlice s) {return o << fleece::slice(s);}
static inline std::ostream& operator<< (std::ostream& o, FLSliceResult s) {return o << fleece::slice(s);}

std::string sliceToHex(fleece::slice);
std::string sliceToHexDump(fleece::slice, size_t width = 16);

// Converts JSON5 to JSON; helps make JSON test input more readable!
std::string json5(std::string_view);
fleece::alloc_slice json5slice(std::string_view);
