//
// LoggingReporter.hh
//
// Copyright (C) 2020 Jens Alfke. All Rights Reserved.
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
#include "CaseListReporter.hh"
#include "MultiLogDecoder.hh"

#include "c4Base.h"
#include "c4Private.h"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <fstream>
#include <optional>

#ifdef _MSC_VER
#include <atlbase.h>
#endif


struct LoggingReporter: public CaseListReporter {

    static std::string getDescription() {
        return "Suppresses LiteCore logging until a test assertion fails";
    }

    LoggingReporter( Catch::ReporterConfig const& config )
    :CaseListReporter(config)
    {
        fleece::alloc_slice buildInfo = c4_getBuildInfo();
        fleece::alloc_slice version = c4_getVersion();
        C4Log("This is LiteCore %.*s ... short version %.*s", SPLAT(buildInfo), SPLAT(version));

        if (c4log_binaryFileLevel() == kC4LogNone) {
            auto folderName = litecore::format("LiteCore_Test_Logs_%lld/",
                                               std::chrono::milliseconds(time(nullptr)).count());
            _logDir = getTempDirectory()[folderName];
            _logDir->mkdir();
            std::string path = _logDir->path();
            C4Log("Beginning binary logging to %s", path.c_str());
            C4Error error;
            if (!c4log_writeToBinaryFile({kC4LogVerbose, c4str(path.c_str()), 16*1024, 1, false},
                                         &error)) {
                C4Log("*** ERROR: Can't log to binary file");
            }
            c4log_setBinaryFileLevel(kC4LogVerbose);
            c4log_setCallbackLevel(kC4LogWarning);

            litecore::error::setNotableExceptionHook([=]() { dumpBinaryLogs(); });
        } else {
            C4Log("LoggingReporter: Binary logging is already enabled, so I'm not doing it");
        }

        c4log_enableFatalExceptionBacktrace();

        sInstance = this;
    }


    virtual ~LoggingReporter() {
        if (_logDir)
            _logDir->delRecursive();

        if (sInstance == this)
            sInstance = nullptr;
    }


    static void dumpLogsNow() {
        sInstance->dumpBinaryLogs();
    }


    //---- Catch overrides

    virtual void testCaseStarting( Catch::TestCaseInfo const& testInfo ) override {
        _caseStartTime = litecore::LogIterator::now();
        c4log_warnOnErrors(true);
        CaseListReporter::testCaseStarting(testInfo);
    }


    virtual void sectionStarting( Catch::SectionInfo const& sectionInfo ) override {
        _caseStartTime = litecore::LogIterator::now();
        CaseListReporter::sectionStarting(sectionInfo);
    }


    virtual bool assertionEnded( Catch::AssertionStats const& assertionStats ) override {
        if (!assertionStats.assertionResult.isOk())
            dumpBinaryLogs();
        return CaseListReporter::assertionEnded(assertionStats);
    }


private:
    static litecore::FilePath getTempDirectory() {
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


    void dumpBinaryLogs() {
        if (_logDir && c4log_callbackLevel() == kC4LogWarning) {
            c4log_flushLogFiles();

            litecore::MultiLogDecoder multi;
            _logDir->forEachFile([&](const litecore::FilePath &item) {
                if (item.extension() == ".cbllog") {
                    if (!multi.add(item.path()))
                        Warn("LoggingReporter: Can't open log file %s", item.path().c_str());
                }
            });

            multi.decodeTo(std::cerr, {"***", "", "", "WARNING", "ERROR"}, _caseStartTime);
        }
        _caseStartTime = litecore::LogIterator::now();
    }


    static inline LoggingReporter* sInstance {nullptr};

    std::optional<litecore::FilePath> _logDir;
    litecore::LogIterator::Timestamp _caseStartTime;
};

CATCH_REGISTER_REPORTER( "logging", LoggingReporter )
