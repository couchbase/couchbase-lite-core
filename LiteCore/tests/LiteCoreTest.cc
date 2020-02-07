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
#include "Backtrace.hh"
#include "Encoder.hh"
#include <csignal>
#include <stdlib.h>
#include <stdarg.h>
#include <mutex>
#include <thread>
#include <chrono>
#include "SecureRandomize.hh"
#include "TempArray.hh"

#if TARGET_OS_IPHONE
#include <CoreFoundation/CFBundle.h>
#endif

#ifdef _MSC_VER
    #undef min
#endif


using namespace std;

#if defined(CMAKE) && defined(_MSC_VER)
string TestFixture::sFixturesDir = "../LiteCore/tests/data/";
#else
string TestFixture::sFixturesDir = "LiteCore/tests/data/";
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


#pragma mark - TESTFIXTURE:


static atomic_uint sWarningsLogged;


static void logCallback(const LogDomain &domain, LogLevel level,
                        const char *fmt, va_list args)
{
    LogDomain::defaultCallback(domain, level, fmt, args);
    if (level >= LogLevel::Warning)
        ++sWarningsLogged;
}


TestFixture::TestFixture()
:_warningsAlreadyLogged(sWarningsLogged)
,_objectCount(c4_getObjectCount())
{
    static once_flag once;
    call_once(once, [] {
        Backtrace::installTerminateHandler(nullptr);

        C4StringResult version = c4_getBuildInfo();
        Log("This is LiteCore %.*s", SPLAT(version));

        LogDomain::setCallback(&logCallback, false);
        if (LogDomain::fileLogLevel() == LogLevel::None) {
            auto path = FilePath::tempDirectory()["LiteCoreC++Tests.c4log"];
            Log("Beginning logging to %s", path.path().c_str());
            LogFileOptions fileOptions { path.path(), LogLevel::Verbose, 1024, 1, false };
            LogDomain::writeEncodedLogsTo(fileOptions,
                                          format("LiteCore %.*s", SPLAT(version)));
        }
        if (getenv("LiteCoreTestsQuiet"))
            LogDomain::setCallbackLogLevel(LogLevel::Warning);
        c4slice_free(version);

#if TARGET_OS_IPHONE
        // iOS tests copy the fixture files into the test bundle.
        CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR("org.couchbase.LiteCoreTests"));
        CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
        CFStringRef path = CFURLCopyPath(url);
        CFRelease(url);
        char buf[1024];
        Assert(CFStringGetCString(path, buf, sizeof(buf), kCFStringEncodingUTF8));
        sFixturesDir = string(buf) + "TestData/LiteCore/tests/data/";
        CFRelease(path);
#endif

    });
    error::sWarnOnError = true;
}


TestFixture::~TestFixture() {
#if ATOMIC_INT_LOCK_FREE > 1
    if (!current_exception()) {
        // Check for leaks:
        int leaks;
        int attempt = 0;
        while ((leaks = c4_getObjectCount() - _objectCount) > 0 && attempt++ < 10) {
            this_thread::sleep_for(chrono::microseconds(200000)); // wait up to 2 seconds for bg threads to free objects
        }
        if (leaks > 0) {
            fprintf(stderr, "*** LEAKED LITECORE OBJECTS: \n");
            c4_dumpInstances();
            fprintf(stderr, "***\n");
        }
        CHECK(leaks == 0);
    }
#endif
}


unsigned TestFixture::warningsLogged() noexcept {
    return sWarningsLogged - _warningsAlreadyLogged;
}

FilePath TestFixture::GetPath(const string& name, const string& extension) noexcept {
    static chrono::milliseconds unique;

    static once_flag f;
    call_once(f, [=] {
        unique = chrono::milliseconds(time(nullptr));
    });

    const char* trimmedExtension = !extension.empty() && extension[0] == '.' ? extension.c_str() + 1 : extension.c_str();
    TempArray(folderName, char, name.size() + 32);
    sprintf(folderName, "%s%lld.%s", name.c_str(), unique.count(), trimmedExtension);

    const auto base = FilePath::tempDirectory()[(const char *)folderName];

    return base;   
}



#pragma mark - DATAFILETESTFIXTURE:


DataFile::Factory& DataFileTestFixture::factory() {
    return SQLiteDataFile::sqliteFactory();
}


FilePath DataFileTestFixture::databasePath(const string baseName) {
    return GetPath(baseName, factory().filenameExtension());
}


/*static*/ void DataFileTestFixture::deleteDatabase(const FilePath &dbPath) {
    auto factory = DataFile::factoryForFile(dbPath);
    factory->deleteFile(dbPath);
}

DataFile* DataFileTestFixture::newDatabase(const FilePath &path, const DataFile::Options *options) {
    //TODO: Set up options
    return factory().openFile(path, this, options);
}


void DataFileTestFixture::reopenDatabase(const DataFile::Options *newOptions) {
    auto dbPath = db->filePath();
    auto options = db->options();
    Debug("//// Closing db");
    db.reset();
    store = nullptr;
    Debug("//// Reopening db");
    db.reset(newDatabase(dbPath, newOptions ? newOptions : &options));
    store = &db->defaultKeyStore();
}


DataFileTestFixture::DataFileTestFixture(int testOption, const DataFile::Options *options) {
    auto dbPath = databasePath("cbl_core_temp");
    deleteDatabase(dbPath);
    db.reset(newDatabase(dbPath, options));
    store = &db->defaultKeyStore();
}


void DataFileTestFixture::deleteDatabase() {
    try {
        db->deleteDataFile();
    } catch (...) { }
    db.reset();
}


DataFileTestFixture::~DataFileTestFixture() {
    if (db)
        deleteDatabase();
}



sequence_t DataFileTestFixture::writeDoc(slice docID,
                                         DocumentFlags flags,
                                         Transaction &t,
                                         function<void(fleece::impl::Encoder&)> writeProperties)
{
    fleece::impl::Encoder enc;
    enc.beginDictionary();
    writeProperties(enc);
    enc.endDictionary();
    alloc_slice body = enc.finish();
    return store->set(docID, nullslice, body, flags, t);
}


slice DataFileTestFixture::fleeceAccessor(slice recordBody) const {
    return recordBody;
}

alloc_slice DataFileTestFixture::blobAccessor(const fleece::impl::Dict*) const {
    return {};
}
