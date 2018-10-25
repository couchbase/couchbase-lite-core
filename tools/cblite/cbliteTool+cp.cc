//
// cbliteTool+cp.cc
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
#include "Endpoint.hh"
#include "DBEndpoint.hh"
#include "CivetWebSocket.hh"
#include "Stopwatch.hh"


const Tool::FlagSpec CBLiteTool::kCpFlags[] = {
    {"--bidi",      (FlagHandler)&CBLiteTool::bidiFlag},
    {"--continuous",(FlagHandler)&CBLiteTool::continuousFlag},
    {"--limit",     (FlagHandler)&CBLiteTool::limitFlag},
    {"--existing",  (FlagHandler)&CBLiteTool::existingFlag},
    {"-x",          (FlagHandler)&CBLiteTool::existingFlag},
    {"--jsonid",    (FlagHandler)&CBLiteTool::jsonIDFlag},
    {"--careful",   (FlagHandler)&CBLiteTool::carefulFlag},
    {"--user",      (FlagHandler)&CBLiteTool::userFlag},
    {"--verbose",   (FlagHandler)&CBLiteTool::verboseFlag},
    {"-v",          (FlagHandler)&CBLiteTool::verboseFlag},
    {nullptr, nullptr}
};

void CBLiteTool::cpUsage() {
    cerr << ansiBold();
    if (!_interactive)
        cerr << "cblite ";
    cerr << "cp" << ' ' << ansiItalic();
    cerr << "[FLAGS]" << ' ';
    if (!_interactive)
        cerr << "SOURCE ";
    cerr <<
    "DESTINATION" << ansiReset() << "\n"
    "  Copies local and remote databases and JSON files.\n"
    "    --existing or -x : Fail if DESTINATION doesn't already exist.\n"
    "    --jsonid <property> : When SOURCE is JSON, this is a property name/path whose value will\n"
    "           be used as the docID. (If omitted, documents are given UUIDs.)\n"
    "           When DESTINATION is JSON, this is a property name that will be added to the JSON, whose\n"
    "           value is the docID. (If omitted, defaults to \"_id\".)\n"
    "    --bidi : Bidirectional (push+pull) replication.\n"
    "    --continuous : Continuous replication.\n"
    "    --user <name>[:<password>] : Credentials for remote database. (If password is not given,\n"
    "           the tool will prompt you to enter it.)\n"
    "    --limit <n> : Stop after <n> documents. (Replicator ignores this)\n"
    "    --careful : Abort on any error.\n"
    "    --verbose or -v : Display progress; repeat flag for more verbosity.\n"
    "    " << it(_interactive ? "DESTINATION" : "SOURCE, DESTINATION")
           << " : Database path, replication URL, or JSON file path\n"
    "    Modes:\n"
    "        *.cblite2 <--> *.cblite2 :  Local replication\n"
    "        *.cblite2 <--> ws://*    :  Networked replication\n"
    "        *.cblite2 <--> *.json    :  Imports/exports JSON file (one doc per line)\n"
    "        *.cblite2 <--> */        :  Imports/exports directory of JSON files (one per doc)\n";
    if (_interactive) {
        cerr <<
        "    Synonyms are \"push\", \"export\", \"pull\", \"import\" (in the latter two, the parameter\n"
        "    is the SOURCE while the current database is the DESTINATION.)\n";
    }
}


void CBLiteTool::copyDatabase(bool reversed) {
    // Read params:
    processFlags(kCpFlags);
    if (_showHelp) {
        cpUsage();
        return;
    }

    if (verbose() >= 2) {
        c4log_setCallbackLevel(kC4LogInfo);
        auto syncLog = c4log_getDomain("Sync", true);
        c4log_setLevel(syncLog, max(kC4LogDebug, C4LogLevel(kC4LogInfo - verbose() + 2)));
    }

    RegisterC4CivetWebSocketFactory();

    const char *firstArgName = "source path/URL", *secondArgName = "destination path/URL";
    if (reversed)
        swap(firstArgName, secondArgName);

    unique_ptr<Endpoint> src(_db ? Endpoint::create(_db)
                                 : Endpoint::create(nextArg(firstArgName)));
    unique_ptr<Endpoint> dst(Endpoint::create(nextArg(secondArgName)));
    if (argCount() > 0)
        fail("Too many arguments");

    if (reversed)
        swap(src, dst);

    bool replicating = (src->isDatabase() && dst->isDatabase());
    if (replicating) {
        auto localDB = dynamic_cast<DbEndpoint*>(src.get());
        if (!localDB)
            localDB = dynamic_cast<DbEndpoint*>(dst.get());
        localDB->setBidirectional(_bidi);
        localDB->setContinuous(_continuous);

        if (!_user.empty()) {
            string user;
            string password;
            auto colon = _user.find(':');
            if (colon != string::npos) {
                password = _user.substr(colon+1);
                user = _user.substr(0, colon);
            } else {
                user = _user;
                password = readPassword(("Server password for " + user + ": ").c_str());
            }
            localDB->setCredentials({user, password});
        }
    } else {
        if (_bidi || _continuous || !_user.empty())
            fail("--bidi, --continuous and --user flags only apply to replication");
    }

    if (_currentCommand == "push" || _currentCommand == "pull") {
        if (!replicating)
            fail("Push/pull must be between databases, not JSON");
    } else if (_currentCommand == "import" || _currentCommand == "export") {
        if (replicating)
            fail("Import/export must specify a JSON file/directory");
    }

    copyDatabase(src.get(), dst.get());
}


void CBLiteTool::copyDatabase(Endpoint *src, Endpoint *dst) {
    src->prepare(true, true, _jsonIDProperty, dst);
    dst->prepare(false, !_createDst, _jsonIDProperty, src);

    Stopwatch timer;
    src->copyTo(dst, _limit);
    dst->finish();

    double time = timer.elapsed();
    cout << "Completed " << dst->docCount() << " docs in " << time << " secs; "
         << int(dst->docCount() / time) << " docs/sec\n";
}


