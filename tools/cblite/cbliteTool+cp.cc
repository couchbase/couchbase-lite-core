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
    {"--replicate", (FlagHandler)&CBLiteTool::replicateFlag},
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
    "    --replicate : Forces use of replicator, for local-to-local db copy\n"
    "    --verbose or -v : Display progress; repeat flag for more verbosity.\n"
    "    " << it(_interactive ? "DESTINATION" : "SOURCE, DESTINATION")
           << " : Database path, replication URL, or JSON file path\n"
    "    Modes:\n"
    "        *.cblite2 <--> *.cblite2 :  Copies local db file, and assigns new UUID to target\n"
    "        *.cblite2 <--> *.cblite2 :  With --replicate flag, runs local replication [EE]\n"
    "        *.cblite2 <--> ws://*    :  Networked replication\n"
    "        *.cblite2 <--> *.json    :  Imports/exports JSON file (one doc per line)\n"
    "        *.cblite2 <--> */        :  Imports/exports directory of JSON files (one per doc)\n";
    if (_interactive) {
        cerr <<
        "    Synonyms are \"push\", \"export\", \"pull\", \"import\".\n"
        "    With \"pull\" and \"import\", the parameter is the SOURCE while the current database\n"
        "    is the DESTINATION.\n"
        "    \"push\" and \"pull\" always replicate, as though --replicate were given.\n"
        ;
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

    bool dbToDb = (src->isDatabase() && dst->isDatabase());
    bool copyLocalDBs = false;

    if (_currentCommand == "push" || _currentCommand == "pull"
            || _bidi || _continuous || !_user.empty()
            || src->isRemote() || dst->isRemote())
        _replicate = true;
    if (_replicate) {
        if (_currentCommand == "import" || _currentCommand == "export")
            failMisuse("'import' and 'export' do not support replication");
        if (!dbToDb)
            failMisuse("Replication is only possible between two databases");
        auto localDB = dynamic_cast<DbEndpoint*>(src.get());
        if (!localDB)
            localDB = dynamic_cast<DbEndpoint*>(dst.get());
        if (!localDB)
            failMisuse("Replication requires at least one database to be local");
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
        copyLocalDBs = dbToDb;
    }

    if (copyLocalDBs)
        copyLocalToLocalDatabase((DbEndpoint*)src.get(), (DbEndpoint*)dst.get());
    else
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


void CBLiteTool::copyLocalToLocalDatabase(DbEndpoint *src, DbEndpoint *dst) {
    alloc_slice dstPath = dst->path();
    if (verbose())
        cout << "Copying to " << dstPath << " ...\n";

    C4DatabaseConfig config = { kC4DB_Create | kC4DB_AutoCompact | kC4DB_SharedKeys };

    Stopwatch timer;
    C4Error error;
    if (!c4db_copy(src->path(), dstPath, &config, &error))
        Tool::instance->errorOccurred("copying database", error);
    double time = timer.elapsed();
    cout << "Completed copy in " << time << " secs\n";
}
