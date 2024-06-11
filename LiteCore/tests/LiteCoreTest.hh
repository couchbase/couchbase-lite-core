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

#include "fleece/PlatformCompat.hh"
#include "TestsCommon.hh"
#include "c4Base.hh"
#include "Error.hh"
#include "fleece/function_ref.hh"
#include "Logging.hh"
#include <array>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#ifdef DEBUG
#    define CHECK_IF_DEBUG   CHECK
#    define REQUIRE_IF_DEBUG REQUIRE
#else
#    define CHECK_IF_DEBUG(x)
#    define REQUIRE_IF_DEBUG(x)
#endif

namespace fleece::impl {
    class Dict;
    class Encoder;
}  // namespace fleece::impl

using namespace fleece;


std::string stringWithFormat(const char* format, ...) __printflike(1, 2);

/**
 * Generate a string of the given number of random decimal digits.
 * (Not technically random, based on time).
 * @tparam numDigits The number of random digits to generate (must be even and <= 64).
 * @return A generated string of random digits.
 */
template <size_t numDigits>
static std::string randomDigitString() {
    static_assert(1 < numDigits <= 64);
    static_assert(numDigits % 2 == 0);
    auto appendEightDigits = [](std::stringstream& sstr) {
        auto now     = std::chrono::high_resolution_clock::now();
        auto epoch   = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
        sstr << std::setfill('0') << std::setw(8) << (seconds % 100000000);
    };
    std::stringstream sstr;
    for ( int i = 0; i < numDigits; i += 8 ) { appendEightDigits(sstr); }
    return sstr.str();
}

/**
 * Generate an array of random digit strings.
 * @tparam count The number of strings to generate.
 * @tparam numDigits The number of decimal digits per string.
 * @return An array of generated strings of random digits.
 */
template <size_t count, size_t numDigits>
static std::array<std::string, count> randomDigitStrings() {
    std::array<std::string, count> arr;
    for ( size_t i = 0; i < count; i++ ) { arr[i] = randomDigitString<numDigits>(); }
    return arr;
}

// The lambda must throw a litecore::error with the given domain and code, or the test fails.
void ExpectException(litecore::error::Domain, int code, const std::function<void()>& lambda);
// In this variant the exception message must match too, unless `what` is nullptr.
void ExpectException(litecore::error::Domain domain, int code, const char* what, const std::function<void()>& lambda);

#include "CatchHelper.hh"

#include "DataFile.hh"

using namespace litecore;

class TestFixture {
  public:
             TestFixture();
    virtual ~TestFixture();

    unsigned                  warningsLogged() const noexcept;
    static litecore::FilePath GetPath(const std::string& name, const std::string& extension) noexcept;

    static std::string sFixturesDir;
    static FilePath    sTempDir;

  private:
    unsigned const _warningsAlreadyLogged;
    int            _objectCount;
};

class DataFileTestFixture
    : public TestFixture
    , public DataFile::Delegate {
  public:
    static const int numberOfOptions = 1;

    DataFileTestFixture() : DataFileTestFixture(0) {}  // defaults to SQLite, rev-trees

    explicit DataFileTestFixture(int testOption, const DataFile::Options* options = nullptr);

    ~DataFileTestFixture() override;

    static DataFile::Factory& factory();

    std::unique_ptr<DataFile> db;
    KeyStore*                 store{nullptr};

    FilePath    databasePath();
    void        deleteDatabase();
    static void deleteDatabase(const FilePath& dbPath);
    DataFile*   newDatabase(const FilePath& path, const DataFile::Options* = nullptr);
    void        reopenDatabase(const DataFile::Options* newOptions = nullptr);

    static sequence_t createDoc(KeyStore&, slice docID, slice body, ExclusiveTransaction&);

    sequence_t createDoc(slice docID, slice body, ExclusiveTransaction& t) const {
        return createDoc(*store, docID, body, t);
    };

    sequence_t writeDoc(slice docID, DocumentFlags flags, ExclusiveTransaction& t,
                        std::function<void(fleece::impl::Encoder&)> fn, bool inOuterDict = true) {
        return writeDoc(*store, docID, flags, t, std::move(fn), inOuterDict);
    }

    static sequence_t writeDoc(KeyStore&, slice docID, DocumentFlags, ExclusiveTransaction&,
                               const std::function<void(fleece::impl::Encoder&)>&, bool inOuterDict = true);

    [[nodiscard]] string databaseName() const override { return _databaseName; }

    alloc_slice blobAccessor(const fleece::impl::Dict*) const override;

    string _databaseName{"db"};
};
