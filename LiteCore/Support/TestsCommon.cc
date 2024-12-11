//
// TestsCommon.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Base.h"
#include "c4Log.h"
#include "Error.hh"
#include "FilePath.hh"
#include "TestsCommon.hh"  // iwyu pragma: keep
#include "fleece/PlatformCompat.hh"
#include "StringUtil.hh"
#include "fleece/FLExpert.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#    include <atlbase.h>
#endif

#include "catch.hpp"


using namespace std;
using namespace litecore;
using namespace fleece;

FilePath GetSystemTempDirectory() {
#ifdef _MSC_VER
    WCHAR pathBuffer[MAX_PATH + 1];
    GetTempPathW(MAX_PATH, pathBuffer);
    GetLongPathNameW(pathBuffer, pathBuffer, MAX_PATH);
    CW2AEX<256> convertedPath(pathBuffer, CP_UTF8);
    return litecore::FilePath(convertedPath.m_psz, "");
#else   // _MSC_VER
    const char* tmp = getenv("TMPDIR");
    if ( !tmp ) tmp = "/tmp";
    return {tmp, ""};
#endif  // _MSC_VER
}

FilePath GetTempDirectory() {
    static FilePath  kTempDir;
    static once_flag f;
    call_once(f, [=] {
        constexpr size_t bufSize = 64;
        char             folderName[bufSize];
        snprintf(folderName, bufSize, "LiteCore_Tests_%" PRIms ".cblite2/",
                 chrono::milliseconds(time(nullptr)).count());
        kTempDir = GetSystemTempDirectory()[folderName];
        (void)kTempDir.mkdir();  // it's OK if it already exists
    });

    return kTempDir;
}

void InitTestLogging() {
    static once_flag once;
    call_once(once, [] {
        c4log_writeToCallback(kC4LogInfo, c4log_defaultCallback, false);

        alloc_slice buildInfo = c4_getBuildInfo();
        alloc_slice version   = c4_getVersion();
        C4Log("This is LiteCore %.*s ... short version %.*s", SPLAT(buildInfo), SPLAT(version));

        if ( c4log_binaryFileLevel() == kC4LogNone ) {
            auto logDir = GetTempDirectory()["binaryLogs/"];
            (void)logDir.mkdir();  // it's OK if it already exists
            string path = logDir.path();
            C4Log("Beginning binary logging to %s", path.c_str());
            C4Error error;
            if ( !c4log_writeToBinaryFile({kC4LogDebug, slice(path), 16 * 1024, 1, false}, &error) ) {
                C4WarnError("TestsCommon: Can't log to binary file, %.*s", SPLAT(c4error_getDescription(error)));
            }
        } else {
            C4Log("Binary logging is already enabled, so I'm not doing it");
        }

        c4error_setCaptureBacktraces(true);
        c4log_enableFatalExceptionBacktrace();
    });
}

string sliceToHex(pure_slice result) {
    string           hex;
    constexpr size_t bufSize = 4;
    for ( size_t i = 0; i < result.size; i++ ) {
        char str[bufSize];
        snprintf(str, bufSize, "%02X", result[i]);
        hex.append(str);
        if ( i % 2 && i != result.size - 1 ) hex.append(" ");
    }
    return hex;
}

string sliceToHexDump(pure_slice result, size_t width) {
    string           hex;
    constexpr size_t bufSize = 4;
    for ( size_t row = 0; row < result.size; row += width ) {
        size_t end = min(row + width, result.size);
        for ( size_t i = row; i < end; ++i ) {
            char str[bufSize];
            snprintf(str, bufSize, "%02X", result[i]);
            hex.append(str);
            if ( i % 2 && i != result.size - 1 ) hex.append(" ");
        }
        hex.append("    ");
        for ( size_t i = row; i < end; ++i ) {
            char str[2] = {(char)result[i], 0};
            if ( result[i] < 32 || result[i] >= 127 ) str[0] = '.';
            hex.append(str);
        }
        hex.append("\n");
    }
    return hex;
}

namespace fleece {
    ostream& operator<<(ostream& o, pure_slice s) {
        o << "slice[";
        if ( s.buf == nullptr ) return o << "null]";
        auto buf = (const uint8_t*)s.buf;
        for ( size_t i = 0; i < s.size; i++ ) {
            if ( buf[i] < 32 || buf[i] > 126 ) return o << sliceToHex(s) << "]";
        }
        return o << "\"" << string((char*)s.buf, s.size) << "\"]";
    }
}  // namespace fleece

fleece::alloc_slice json5slice(string_view str) {
    FLStringResult errorMsg = {};
    size_t         errorPos = 0;
    FLError        err;
    auto           json = alloc_slice(FLJSON5_ToJSON(slice(str), &errorMsg, &errorPos, &err));
    INFO("JSON5 error: " << string(alloc_slice(errorMsg)) << ", input was: " << str);
    Require(json.buf);
    return json;
}

string json5(string_view str) { return string(json5slice(str)); }

extern "C" CBL_CORE_API std::atomic_int gC4ExpectExceptions;

// While in scope, suppresses warnings about errors, and debugger exception breakpoints (in Xcode)
ExpectingExceptions::ExpectingExceptions() {
    ++gC4ExpectExceptions;
    c4log_warnOnErrors(false);
}

ExpectingExceptions::~ExpectingExceptions() {
    if ( --gC4ExpectExceptions == 0 ) c4log_warnOnErrors(true);
}

bool WaitUntil(chrono::milliseconds timeout, function_ref<bool()> predicate) {
    auto deadline = chrono::steady_clock::now() + timeout;
    do {
        if ( predicate() ) return true;
        this_thread::sleep_for(50ms);
    } while ( chrono::steady_clock::now() < deadline );

    return false;
}

bool ReadFileByLines(const string& path, function_ref<bool(FLSlice)> callback, size_t maxLines) {
    Info("Reading lines from " << path);
    fstream fd(path.c_str(), ios_base::in);
    Require(fd);
    vector<char> buf(1000000);  // The Wikipedia dumps have verrry long lines
    size_t       lineCount = 0;
    while ( fd.good() ) {
        if ( maxLines > 0 && lineCount == maxLines ) { break; }
        // Ensure that buf.capacity (size_t/uint64) will not exceed limit of std::streamsize (int64)
        DebugAssert(buf.capacity() <= std::numeric_limits<std::streamsize>::max());
        fd.getline(buf.data(), buf.capacity());  // NOLINT(cppcoreguidelines-narrowing-conversions)
        auto len = fd.gcount();
        if ( len <= 0 ) break;
        ++lineCount;
        Require(buf[len - 1] == '\0');
        --len;
        if ( !callback({buf.data(), (size_t)len}) ) return false;
    }
    Require((fd.eof() || (maxLines > 0 && lineCount == maxLines)));
    return true;
}

// Static initializer will run on the main thread at startup
static const std::thread::id sMainThreadID = std::this_thread::get_id();

bool OnMainThread() noexcept { return std::this_thread::get_id() == sMainThreadID; }

void C4AssertionFailed(const char* fn, const char* file, unsigned line, const char* expr, const char* message) {
    if ( !message ) message = expr;
    fprintf(stderr, "FATAL: Assertion failed: %s (%s:%u, in %s)\n", message, file, line, fn);
    abort();
}
