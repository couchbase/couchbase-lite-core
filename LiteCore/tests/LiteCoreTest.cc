//
// LiteCoreTest.cc
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

#include "LiteCoreTest.hh"
#include "SQLiteDataFile.hh"
#include "FilePath.hh"
#include "PlatformIO.hh"
#include "StringUtil.hh"
#include "c4Private.h"
#include <stdlib.h>
#include <stdarg.h>
#include <mutex>

#if defined(__linux__)
    #include "arc4random.h"
#endif

#ifdef _MSC_VER
    #include <arc4random.h>
    #undef min
#endif


using namespace std;

#if defined(CMAKE) && defined(_MSC_VER)
string DataFileTestFixture::sFixturesDir = "../LiteCore/tests/data/";
#else
string DataFileTestFixture::sFixturesDir = "LiteCore/tests/data/";
#endif


string stringWithFormat(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *cstr;
    REQUIRE(vasprintf(&cstr, format, args) >= 0);
    va_end(args);
    REQUIRE(cstr);
    string str(cstr);
    free(cstr);
    return str;
}


std::string sliceToHex(slice result) {
    std::string hex;
    for (size_t i = 0; i < result.size; i++) {
        char str[4];
        sprintf(str, "%02X", result[i]);
        hex.append(str);
        if (i % 2 && i != result.size-1)
            hex.append(" ");
    }
    return hex;
}


std::string sliceToHexDump(slice result, size_t width) {
    std::string hex;
    for (size_t row = 0; row < result.size; row += width) {
        size_t end = std::min(row + width, result.size);
        for (size_t i = row; i < end; ++i) {
            char str[4];
            sprintf(str, "%02X", result[i]);
            hex.append(str);
            if (i % 2 && i != result.size-1)
                hex.append(" ");
        }
        hex.append("    ");
        for (size_t i = row; i < end; ++i) {
            char str[2] = {(char)result[i], 0};
            if (result[i] < 32 || result[i] >= 127)
                str[0] = '.';
            hex.append(str);
        }
        hex.append("\n");
    }
    return hex;
}


void randomBytes(slice dst) {
    arc4random_buf((void*)dst.buf, (int)dst.size);
}

namespace fleece {
    std::ostream& operator<< (std::ostream& o, slice s) {
        o << "slice[";
        if (s.buf == nullptr)
            return o << "null]";
        auto buf = (const uint8_t*)s.buf;
        for (size_t i = 0; i < s.size; i++) {
            if (buf[i] < 32 || buf[i] > 126)
                return o << sliceToHex(s) << "]";
        }
        return o << "\"" << std::string((char*)s.buf, s.size) << "\"]";
    }
}


void ExpectException(litecore::error::Domain domain, int code, std::function<void()> lambda) {
    try {
        Log("NOTE: Expecting an exception to be thrown...");
        ++gC4ExpectExceptions;
        error::sWarnOnError = false;
        lambda();
    } catch (std::runtime_error &x) {
        Log("... caught exception %s", x.what());
        --gC4ExpectExceptions;
        error::sWarnOnError = true;
        error err = error::convertRuntimeError(x).standardized();
        CHECK(err.domain == domain);
        CHECK(err.code == code);
        return;
    }
    --gC4ExpectExceptions;
    error::sWarnOnError = true;
    FAIL("Should have thrown an exception");
}


DataFile::Factory& DataFileTestFixture::factory() {
    return SQLiteDataFile::sqliteFactory();
}


FilePath DataFileTestFixture::databasePath(const string baseName) {
    auto path = FilePath::tempDirectory()[baseName];
    return path.addingExtension(factory().filenameExtension());
}


/*static*/ void DataFileTestFixture::deleteDatabase(const FilePath &dbPath) {
    auto factory = DataFile::factoryForFile(dbPath);
    factory->deleteFile(dbPath);
}

DataFile* DataFileTestFixture::newDatabase(const FilePath &path, const DataFile::Options *options) {
    //TODO: Set up options
    return factory().openFile(path, options);
}


void DataFileTestFixture::reopenDatabase(const DataFile::Options *newOptions) {
    auto dbPath = db->filePath();
    auto options = db->options();
    Debug("//// Closing db");
    delete db;
    db = nullptr;
    store = nullptr;
    Debug("//// Reopening db");
    db = newDatabase(dbPath, newOptions ? newOptions : &options);
    store = &db->defaultKeyStore();
}


static atomic_uint sWarningsLogged;


static void logCallback(const LogDomain &domain, LogLevel level,
                        const char *fmt, va_list args)
{
    LogDomain::defaultCallback(domain, level, fmt, args);
    if (level >= LogLevel::Warning)
        ++sWarningsLogged;
}


DataFileTestFixture::DataFileTestFixture(int testOption, const DataFile::Options *options)
:_warningsAlreadyLogged(sWarningsLogged)
{
    static once_flag once;
    call_once(once, [] {
        C4StringResult version = c4_getBuildInfo();
        Log("This is LiteCore %.*s", SPLAT(version));

        LogDomain::setCallback(&logCallback, false);
        if (LogDomain::fileLogLevel() == LogLevel::None) {
            auto path = FilePath::tempDirectory()["LiteCoreC++Tests.c4log"];
            Log("Beginning logging to %s", path.path().c_str());
            LogDomain::writeEncodedLogsTo(path, LogLevel::Verbose,
                                          format("LiteCore %.*s", SPLAT(version)));
        }
        c4slice_free(version);
    });
    error::sWarnOnError = true;

    auto dbPath = databasePath("cbl_core_temp");
    deleteDatabase(dbPath);
    db = newDatabase(dbPath, options);
    store = &db->defaultKeyStore();
}


DataFileTestFixture::~DataFileTestFixture() {
    if (db) {
        try {
            db->deleteDataFile();
        } catch (...) { }
        delete db;
    }
}


unsigned DataFileTestFixture::warningsLogged() noexcept {
    return sWarningsLogged - _warningsAlreadyLogged;
}

