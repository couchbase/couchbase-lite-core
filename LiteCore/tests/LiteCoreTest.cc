//
// LiteCoreTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LiteCoreTest.hh"
#include "TestsCommon.hh"
#include "SQLiteDataFile.hh"
#include "FilePath.hh"
#include "PlatformIO.hh"
#include "StringUtil.hh"
#include "c4Private.h"
#include "Backtrace.hh"
#include "Encoder.hh"
#include "Logging.hh"
#include <csignal>
#include <stdlib.h>
#include <stdarg.h>
#include <mutex>
#include <thread>
#include <chrono>
#include "SecureRandomize.hh"
#include "TempArray.hh"

#if TARGET_OS_IPHONE
#    include <CoreFoundation/CFBundle.h>
#endif

#ifdef _MSC_VER
#    include <atlbase.h>
#endif


using namespace std;

#if defined(CMAKE) && defined(_MSC_VER)
string TestFixture::sFixturesDir = "../LiteCore/tests/data/";
#else
string TestFixture::sFixturesDir = "LiteCore/tests/data/";
#endif


FilePath TestFixture::sTempDir = GetTempDirectory();

string stringWithFormat(const char *format, ...) {
    va_list args;
    va_start(args, format);
    string str = vformat(format, args);
    va_end(args);
    return str;
}

void ExpectException(litecore::error::Domain domain, int code, std::function<void()> lambda) {
    try {
        ExpectingExceptions x;
        Log("NOTE: Expecting an exception to be thrown...");
        lambda();
    } catch ( std::runtime_error &x ) {
        Log("... caught exception %s", x.what());
        error err = error::convertRuntimeError(x).standardized();
        CHECK(err.domain == domain);
        CHECK(err.code == code);
        return;
    }
    FAIL("Should have thrown an exception");
}

#pragma mark - TESTFIXTURE:


static LogDomain::Callback_t sPrevCallback;
static atomic_uint           sWarningsLogged;

static void logCallback(const LogDomain &domain, LogLevel level, const char *fmt, va_list args) {
    sPrevCallback(domain, level, fmt, args);
    if ( level >= LogLevel::Warning ) ++sWarningsLogged;
}

TestFixture::TestFixture() : _warningsAlreadyLogged(sWarningsLogged), _objectCount(c4_getObjectCount()) {
    static once_flag once;
    call_once(once, [] {
        InitTestLogging();

        sPrevCallback = LogDomain::currentCallback();
        LogDomain::setCallback(&logCallback, false);

#if TARGET_OS_IPHONE
        // iOS tests copy the fixture files into the test bundle.
        CFBundleRef bundle = CFBundleGetBundleWithIdentifier(CFSTR("org.couchbase.LiteCoreTests"));
        CFURLRef    url    = CFBundleCopyResourcesDirectoryURL(bundle);
        CFStringRef path   = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFRelease(url);
        char buf[1024];
        Assert(CFStringGetCString(path, buf, sizeof(buf), kCFStringEncodingUTF8));
        sFixturesDir = string(buf) + "/TestData/LiteCore/tests/data/";
        CFRelease(path);
#endif
    });
}

TestFixture::~TestFixture() {
    if ( !current_exception() ) {
        // Check for leaks:
        if ( !WaitUntil(2000ms, [&] { return c4_getObjectCount() - _objectCount == 0; }) ) {
            FAIL_CHECK("LiteCore objects were leaked by this test:");
            fprintf(stderr, "*** LEAKED LITECORE OBJECTS: \n");
            c4_dumpInstances();
            fprintf(stderr, "***\n");
        }
    }
}

unsigned TestFixture::warningsLogged() noexcept { return sWarningsLogged - _warningsAlreadyLogged; }

FilePath TestFixture::GetPath(const string &name, const string &extension) noexcept {
    static chrono::milliseconds unique;

    static once_flag f;
    call_once(f, [=] { unique = chrono::milliseconds(time(nullptr)); });

    const char *trimmedExtension
            = !extension.empty() && extension[0] == '.' ? extension.c_str() + 1 : extension.c_str();
    const size_t bufSize = name.size() + 32;
    TempArray(folderName, char, bufSize);
    snprintf(folderName, bufSize, "%s%" PRIms ".%s", name.c_str(), unique.count(), trimmedExtension);

    const auto base = sTempDir[(const char *)folderName];

    return base;
}

#pragma mark - DATAFILETESTFIXTURE:

DataFile::Factory &DataFileTestFixture::factory() { return SQLiteDataFile::sqliteFactory(); }

FilePath DataFileTestFixture::databasePath(const string baseName) {
    return GetPath(baseName, factory().filenameExtension());
}

/*static*/ void DataFileTestFixture::deleteDatabase(const FilePath &dbPath) {
    auto factory = DataFile::factoryForFile(dbPath);
    factory->deleteFile(dbPath);
}

DataFile *DataFileTestFixture::newDatabase(const FilePath &path, const DataFile::Options *options) {
    //TODO: Set up options
    return factory().openFile(path, this, options);
}

void DataFileTestFixture::reopenDatabase(const DataFile::Options *newOptions) {
    auto dbPath  = db->filePath();
    auto options = db->options();
    WriteDebug("//// Closing db");
    db.reset();
    store = nullptr;
    WriteDebug("//// Reopening db");
    db.reset(newDatabase(dbPath, newOptions ? newOptions : &options));
    store = &db->defaultKeyStore();
}

DataFileTestFixture::DataFileTestFixture(int testOption, const DataFile::Options *options) {
    auto dbPath = databasePath(databaseName());
    deleteDatabase(dbPath);
    db.reset(newDatabase(dbPath, options));
    store = &db->defaultKeyStore();
}

void DataFileTestFixture::deleteDatabase() {
    try {
        db->deleteDataFile();
    } catch ( ... ) {}
    db.reset();
}

DataFileTestFixture::~DataFileTestFixture() {
    if ( db ) deleteDatabase();
}

sequence_t DataFileTestFixture::createDoc(KeyStore &s, slice docID, slice body, ExclusiveTransaction &t) {
    RecordUpdate rec(docID, body);
    auto         seq = s.set(rec, true, t);
    CHECK(seq != 0_seq);
    return seq;
}

sequence_t DataFileTestFixture::writeDoc(KeyStore &toStore, slice docID, DocumentFlags flags, ExclusiveTransaction &t,
                                         function<void(fleece::impl::Encoder &)> writeProperties) {
    fleece::impl::Encoder enc;
    enc.beginDictionary();
    writeProperties(enc);
    enc.endDictionary();
    alloc_slice body = enc.finish();

    if ( toStore.capabilities().sequences ) {
        RecordUpdate rec(docID, body, flags);
        return toStore.set(rec, true, t);
    } else {
        toStore.setKV(docID, body, t);
        return 0_seq;
    }
}

alloc_slice DataFileTestFixture::blobAccessor(const fleece::impl::Dict *) const { return {}; }
