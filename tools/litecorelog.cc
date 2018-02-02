//
// litecorelog.cc
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

#include "c4.hh"
#include "LogDecoder.hh"
#include "StringUtil.hh"
#include <exception>
#include <iostream>
#include <fstream>
#include <vector>

using namespace std;
using namespace litecore;


static const vector<string> kLevels = {"***", "", "", "WARNING", "ERROR"};


int LiteCoreLogMain(vector<string> &args);


[[noreturn]] static void fail(const char *message) {
    cerr << "Error: " << message << "\n";
    exit(1);
}

[[noreturn]] static void fail(const string &message) {
    fail(message.c_str());
}


static void usage() {
    cerr <<
    "litecorelog: Dumps encoded LiteCore log files\n"
    "Usage: litecorelog <logfile>\n"
    ;
}


int LiteCoreLogMain(vector<string> &args) {
    try {
        if (args.empty()) {
            usage();
            return 0;
        }

        ifstream in(args[0], ifstream::in | ifstream::binary);
        if (!in)
            fail(format("Couldn't open input file '%s'", args[0].c_str()));
        in.exceptions(std::ifstream::badbit);
        LogDecoder decoder(in);
        decoder.decodeTo(cout, kLevels);
        return 0;
        
    } catch (const std::exception &x) {
        fail(format("Uncaught C++ exception: %s", x.what()));
    } catch (...) {
        fail("Uncaught unknown C++ exception");
    }
}


int main(int argc, const char * argv[]) {
    vector<string> args;
    args.reserve(argc - 1);
    for(int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return LiteCoreLogMain(args);
}
