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
#include "FilePath.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <mutex>

#ifdef _MSC_VER
#include <atlbase.h>
#endif


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
                C4WarnError("TestsCommon: Can't log to binary file");
            }
            c4log_setCallbackLevel(kC4LogInfo);
        } else {
            C4Log("Binary logging is already enabled, so I'm not doing it");
        }

        c4log_enableFatalExceptionBacktrace();
    });
}
