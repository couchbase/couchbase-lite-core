//
//  cbliteTool+logcat.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "cbliteTool.hh"
#include "LogDecoder.hh"


void CBLiteTool::logcatUsage() {
    cerr << ansiBold();
    if (!_interactive)
        cerr << "cblite ";
    cerr << "logcat" << ' ' << ansiItalic() << "LOGFILE" << ansiReset() << '\n';
    cerr <<
    "  Converts a binary log file to text and writes it to stdout\n"
    ;
}


void CBLiteTool::logcat() {
    // Read params:
    processFlags(nullptr);
    if (_showHelp) {
        logcatUsage();
        return;
    }
    string logPath = nextArg("log file path");

    vector<string> kLevels = {"***", "", "",
        ansiBold() + ansiRed() + "WARNING" + ansiReset(),
        ansiBold() + ansiRed() + "ERROR" + ansiReset()};

    ifstream in(logPath, ifstream::in | ifstream::binary);
    if (!in)
        fail(format("Couldn't open '%s'", logPath.c_str()));
    in.exceptions(std::ifstream::badbit);
    
    LogDecoder decoder(in);
    decoder.decodeTo(cout, kLevels);
}
