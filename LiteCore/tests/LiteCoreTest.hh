//
// LiteCoreTest.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <iostream>
#include "fleece/slice.hh"
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

namespace fleece { namespace impl {
    class Dict;
    class Encoder;
} }
using namespace fleece;


std::string stringWithFormat(const char *format, ...) __printflike(1, 2);

// Converts JSON5 to JSON; helps make JSON test input more readable!
static inline std::string json5(const std::string &s)      {return fleece::ConvertJSON5(s);}

std::string sliceToHex(slice);
std::string sliceToHexDump(slice, size_t width = 16);

// Some operators to make slice work with unit-testing assertions:
// (This has to be declared before including catch.hpp, because C++ sucks)
namespace fleece {
    std::ostream& operator<< (std::ostream& o, slice s);
}


// The lambda must throw a litecore::error with the given domain and code, or the test fails.
void ExpectException(litecore::error::Domain, int code, std::function<void()> lambda);

extern "C" CBL_CORE_API std::atomic_int gC4ExpectExceptions;

// While in scope, suppresses warnings about errors, and debugger exception breakpoints (in Xcode)
struct ExpectingExceptions {
    ExpectingExceptions()    {++gC4ExpectExceptions; litecore::error::sWarnOnError = false;}
    ~ExpectingExceptions()   {--gC4ExpectExceptions; litecore::error::sWarnOnError = true;}
};


#include "CatchHelper.hh"

#include "DataFile.hh"

using namespace litecore;


class TestFixture {
public:
    TestFixture();
    ~TestFixture();

    unsigned warningsLogged() noexcept;
    litecore::FilePath GetPath(const std::string& name, const std::string& extension) noexcept;
    
    static std::string sFixturesDir;
    static FilePath sTempDir;

private:
    unsigned const _warningsAlreadyLogged;
    int _objectCount;
};


class DataFileTestFixture : public TestFixture, public DataFile::Delegate {
public:

    static const int numberOfOptions = 1;

    DataFileTestFixture()   :DataFileTestFixture(0) { }     // defaults to SQLite, rev-trees
    DataFileTestFixture(int testOption, const DataFile::Options *options =nullptr);
    ~DataFileTestFixture();

    DataFile::Factory& factory();
    
    std::unique_ptr<DataFile> db;
    KeyStore *store {nullptr};

    FilePath databasePath(const string baseName);
    void deleteDatabase();
    void deleteDatabase(const FilePath &dbPath);
    DataFile* newDatabase(const FilePath &path, const DataFile::Options* =nullptr);
    void reopenDatabase(const DataFile::Options *newOptions =nullptr);

    sequence_t writeDoc(slice docID, DocumentFlags, Transaction&,
                        std::function<void(fleece::impl::Encoder&)>);

    virtual slice fleeceAccessor(slice recordBody) const override;
    virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
};


