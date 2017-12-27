//
//  LiteCoreTest.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include <iostream>
#include "slice.hh"
#include "PlatformCompat.hh"
#include "Error.hh"
#include "Logging.hh"
#include "JSON5.hh"
#include "c4Base.h"
#include <functional>

#ifdef DEBUG
#define CHECK_IF_DEBUG CHECK
#define REQUIRE_IF_DEBUG REQUIRE
#else
#define CHECK_IF_DEBUG(x)
#define REQUIRE_IF_DEBUG(x)
#endif

using namespace fleece;


std::string stringWithFormat(const char *format, ...) __printflike(1, 2);

// Converts JSON5 to JSON; helps make JSON test input more readable!
static inline std::string json5(const std::string &s)      {return fleece::ConvertJSON5(s);}

std::string sliceToHex(slice);
std::string sliceToHexDump(slice, size_t width = 16);

void randomBytes(slice dst);

// Some operators to make slice work with unit-testing assertions:
// (This has to be declared before including catch.hpp, because C++ sucks)
namespace fleece {
    std::ostream& operator<< (std::ostream& o, slice s);
}


// The lambda must throw a litecore::error with the given domain and code, or the test fails.
void ExpectException(litecore::error::Domain, int code, std::function<void()> lambda);

extern "C" CBL_CORE_API std::atomic_int gC4ExpectExceptions;
#if DEBUG
extern "C" CBL_CORE_API std::atomic_int gC4ForceFailure;
#endif

// While in scope, suppresses warnings about errors, and debugger exception breakpoints (in Xcode)
struct ExpectingExceptions {
    ExpectingExceptions()    {++gC4ExpectExceptions; litecore::error::sWarnOnError = false;}
    ~ExpectingExceptions()   {--gC4ExpectExceptions; litecore::error::sWarnOnError = gC4ExpectExceptions <= 0;}
};

#if DEBUG
// While in scope, trigger forced failures for C API functions
struct ForcingFailures {
    ForcingFailures()       {++gC4ForceFailure; ++gC4ExpectExceptions; litecore::error::sWarnOnError = false;}
    ~ForcingFailures()      {--gC4ForceFailure; --gC4ExpectExceptions; litecore::error::sWarnOnError = gC4ExpectExceptions <= 0;}
};
#endif


#include "CatchHelper.hh"

#include "DataFile.hh"

using namespace litecore;
using namespace std;


class DataFileTestFixture {
public:

    static const int numberOfOptions = 1;

    static std::string sFixturesDir;

    DataFileTestFixture()   :DataFileTestFixture(0) { }     // defaults to SQLite, rev-trees
    DataFileTestFixture(int testOption, const DataFile::Options *options =nullptr);
    ~DataFileTestFixture();

    DataFile::Factory& factory();
    
    DataFile *db {nullptr};
    KeyStore *store {nullptr};

    FilePath databasePath(const string baseName);
    void deleteDatabase(const FilePath &dbPath);
    DataFile* newDatabase(const FilePath &path, const DataFile::Options* =nullptr);
    void reopenDatabase(const DataFile::Options *newOptions =nullptr);


};


