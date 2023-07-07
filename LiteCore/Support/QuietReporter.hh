//
// QuietReporter.hh
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
#include "TestsCommon.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "MultiLogDecoder.hh"
#include "c4Log.h"
#include "CaseListReporter.hh"
#include <fstream>
#include <iostream>

#ifdef _MSC_VER
#    include <atlbase.h>
#endif


/** Custom Catch reporter that suppresses most LiteCore logging to the console, while logging verbosely to
    a binary file. If a test fails, the verbose logs from that test are copied to the console just before
    the Catch logs. */
struct QuietReporter : public CaseListReporter {
    static std::string getDescription() { return "Suppresses most LiteCore logging until a test assertion fails"; }

    explicit QuietReporter(Catch::ReporterConfig const& config) : CaseListReporter(config) {
        InitTestLogging();
        litecore::error::setNotableExceptionHook([=]() { dumpBinaryLogs(); });
        sInstance = this;
    }

    ~QuietReporter() override {
        if ( sInstance == this ) sInstance = nullptr;
    }

    /** Immediately dumps the current test's logs.
        This is currently unused, but could be called by hand from a debugger. */
    static void dumpLogsNow() { sInstance->dumpBinaryLogs(); }

    //---- Catch overrides

    void testCaseStarting(Catch::TestCaseInfo const& testInfo) override {
        c4log_setCallbackLevel(kC4LogWarning);
        c4log_warnOnErrors(true);
        _caseStartTime = litecore::LogIterator::now();
        CaseListReporter::testCaseStarting(testInfo);
    }

    void sectionStarting(Catch::SectionInfo const& sectionInfo) override {
        _caseStartTime = litecore::LogIterator::now();
        CaseListReporter::sectionStarting(sectionInfo);
    }

    bool assertionEnded(Catch::AssertionStats const& assertionStats) override {
        if ( !assertionStats.assertionResult.isOk() ) dumpBinaryLogs();
        return CaseListReporter::assertionEnded(assertionStats);
    }


  private:
    void dumpBinaryLogs() {
        fleece::alloc_slice logPath = c4log_binaryFilePath();
        if ( logPath && c4log_binaryFileLevel() < c4log_callbackLevel() ) {
            c4log_flushLogFiles();

            litecore::MultiLogDecoder multi;
            litecore::FilePath        logDir(std::string(logPath), "");
            logDir.forEachFile([&](const litecore::FilePath& item) {
                if ( item.extension() == ".cbllog" ) {
                    if ( !multi.add(item.path()) ) C4Warn("QuietReporter: Can't open log file %s", item.path().c_str());
                }
            });

            std::cerr << "////////// Replaying binary logs... //////////\n";
            multi.decodeTo(std::cerr, {"***", "", "", "WARNING", "ERROR"}, _caseStartTime);
        }
        _caseStartTime = litecore::LogIterator::now();
    }

    static inline QuietReporter* sInstance{nullptr};

    litecore::LogIterator::Timestamp _caseStartTime{};
};


CATCH_REGISTER_REPORTER("quiet", QuietReporter)
