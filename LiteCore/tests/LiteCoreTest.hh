//
// LiteCoreTest.hh
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

#include "PlatformCompat.hh"
#include "TestsCommon.hh"
#include "c4Base.hh"
#include "Error.hh"
#include "Logging.hh"
#include <functional>
#include <memory>
#include <utility>

#ifdef DEBUG
#define CHECK_IF_DEBUG CHECK
#define REQUIRE_IF_DEBUG REQUIRE
#else
#define CHECK_IF_DEBUG(x)
#define REQUIRE_IF_DEBUG(x)
#endif

namespace fleece::impl {
    class Dict;
    class Encoder;
}
using namespace fleece;


std::string stringWithFormat(const char *format, ...) __printflike(1, 2);


// The lambda must throw a litecore::error with the given domain and code, or the test fails.
void ExpectException(litecore::error::Domain, int code, std::function<void()> lambda);


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

    static sequence_t createDoc(KeyStore&, slice docID, slice body, ExclusiveTransaction&);
    sequence_t createDoc(slice docID, slice body, ExclusiveTransaction &t)
                                                    {return createDoc(*store, docID, body, t);};

    sequence_t writeDoc(slice docID, DocumentFlags flags, ExclusiveTransaction &t,
                        std::function<void(fleece::impl::Encoder&)> fn)
                                                    {return writeDoc(*store, docID, flags, t, fn);}
    sequence_t writeDoc(KeyStore&,
                        slice docID, DocumentFlags, ExclusiveTransaction&,
                        std::function<void(fleece::impl::Encoder&)>);

    virtual string databaseName() const override        {return "cbl_core_temp";}
    virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
};


