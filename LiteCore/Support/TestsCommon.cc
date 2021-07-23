//
// TestsCommon.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "TestsCommon.hh"
#include "c4Base.h"
#include "c4Private.h"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.h"
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#include <atlbase.h>
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
#else // _MSC_VER
    return litecore::FilePath("/tmp", "");
#endif // _MSC_VER
}


FilePath GetTempDirectory() {
    static FilePath kTempDir;
    static once_flag f;
    call_once(f, [=] {
        char folderName[64];
        sprintf(folderName, "LiteCore_Tests_%" PRIms "/", chrono::milliseconds(time(nullptr)).count());
        kTempDir = GetSystemTempDirectory()[folderName];
        kTempDir.mkdir();
    });

    return kTempDir;
}


void InitTestLogging() {
    static once_flag once;
    call_once(once, [] {
        alloc_slice buildInfo = c4_getBuildInfo();
        alloc_slice version = c4_getVersion();
        C4Log("This is LiteCore %.*s ... short version %.*s", SPLAT(buildInfo), SPLAT(version));

        if (c4log_binaryFileLevel() == kC4LogNone) {
            auto logDir = GetTempDirectory()["binaryLogs/"];
            logDir.mkdir();
            string path = logDir.path();
            C4Log("Beginning binary logging to %s", path.c_str());
            C4Error error;
            if (!c4log_writeToBinaryFile({kC4LogVerbose, slice(path), 16*1024, 1, false},
                                         &error)) {
                C4WarnError("Can't log to binary files, %.*s",
                            SPLAT(c4error_getDescription(error)));
            }
            c4log_setCallbackLevel(kC4LogInfo);
        } else {
            C4Log("Binary logging is already enabled, so I'm not doing it");
        }

        c4error_setCaptureBacktraces(true);
        c4log_enableFatalExceptionBacktrace();
    });
}


string sliceToHex(pure_slice result) {
    string hex;
    for (size_t i = 0; i < result.size; i++) {
        char str[4];
        sprintf(str, "%02X", result[i]);
        hex.append(str);
        if (i % 2 && i != result.size-1)
            hex.append(" ");
    }
    return hex;
}


string sliceToHexDump(pure_slice result, size_t width) {
    string hex;
    for (size_t row = 0; row < result.size; row += width) {
        size_t end = min(row + width, result.size);
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
    ostream& operator<< (ostream& o, pure_slice s) {
        o << "slice[";
        if (s.buf == nullptr)
            return o << "null]";
        auto buf = (const uint8_t*)s.buf;
        for (size_t i = 0; i < s.size; i++) {
            if (buf[i] < 32 || buf[i] > 126)
                return o << sliceToHex(s) << "]";
        }
        return o << "\"" << string((char*)s.buf, s.size) << "\"]";
    }
}


fleece::alloc_slice json5slice(string_view str) {
    FLStringResult errorMsg = {};
    size_t errorPos = 0;
    FLError err;
    auto json = alloc_slice(FLJSON5_ToJSON(slice(str), &errorMsg, &errorPos, &err));
    INFO("JSON5 error: " << string(alloc_slice(errorMsg)) << ", input was: " << str);
    REQUIRE(json.buf);
    return json;
}


string json5(string_view str) {
    return string(json5slice(str));
}


extern "C" CBL_CORE_API std::atomic_int gC4ExpectExceptions;

// While in scope, suppresses warnings about errors, and debugger exception breakpoints (in Xcode)
ExpectingExceptions::ExpectingExceptions()    {
    ++gC4ExpectExceptions;
    c4log_warnOnErrors(false);
}

ExpectingExceptions::~ExpectingExceptions()   {
    if (--gC4ExpectExceptions == 0)
        c4log_warnOnErrors(true);
}


bool WaitUntil(chrono::milliseconds timeout, function_ref<bool()> predicate) {
    auto deadline = chrono::steady_clock::now() + timeout;
    do {
        if (predicate())
            return true;
        this_thread::sleep_for(50ms);
    } while (chrono::steady_clock::now() < deadline);

    return false;
}
