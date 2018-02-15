//
// cbliteTool+serve.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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
#include <signal.h>
#include <thread>


static constexpr int kDefaultPort = 59840;

static bool gStop = false;


void CBLiteTool::serveUsage() {
    writeUsageCommand("serve", false);
    cerr <<
    "  Runs a REST API server\n"
    "    --port N : Sets TCP port number (default "<<kDefaultPort<<")\n"
    "    --readonly : Prevents REST calls from altering the database\n"
    "    --verbose or -v : Log requests; repeat flag for more verbosity.\n"
    "  Note: Only a subset of the Couchbase Lite REST API is implemented so far.\n"
    "        See <github.com/couchbase/couchbase-lite-core/wiki/REST-API>\n"
    ;
}


static alloc_slice databaseNameFromPath(slice path) {
    c4::stringResult nameSlice(c4db_URINameFromPath(path));
    return alloc_slice(nameSlice);
}


void CBLiteTool::startListener() {
    if (!_listener) {
        C4Error err;
        _listener = c4listener_start(&_listenerConfig, &err);
        if (!_listener)
            fail("starting REST listener", err);
    }
}


void CBLiteTool::serve() {
    // Default configuration (everything else is false/zero):
    _listenerConfig.port = kDefaultPort;
    _listenerConfig.apis = kC4RESTAPI;
    _listenerConfig.allowPush = true;

    // Unlike other subcommands, this one opens the db read/write, unless --readonly is specified
    _dbFlags = _dbFlags & ~kC4DB_ReadOnly;

    // Read params:
    processFlags(kServeFlags);
    if (_showHelp) {
        serveUsage();
        return;
    }

    if (_dbFlags & kC4DB_ReadOnly)
        _listenerConfig.allowPull = true;

    openDatabaseFromNextArg();
    endOfArgs();

    c4::stringResult dbPath(c4db_getPath(_db));
    alloc_slice name = databaseNameFromPath(dbPath);

    c4log_setCallbackLevel(kC4LogInfo);
    auto restLog = c4log_getDomain("REST", true);
    c4log_setLevel(restLog, max(kC4LogDebug, C4LogLevel(kC4LogInfo - verbose())));

    startListener();
    
    c4listener_shareDB(_listener, name, _db);

    cout << "LiteCore REST server is now listening at " << ansiBold() << ansiUnderline()
         << "http://localhost:" << _listenerConfig.port << "/" << name << "/" << ansiReset() << "\n";

#ifndef _MSC_VER
    // Run until the process receives SIGINT (^C) or SIGHUP:
    struct sigaction action = {{[](int s) {gStop = true;}}, 0, SA_RESETHAND};
    sigaction(SIGHUP, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    cout << it("(Press ^C to stop)\n");
    while(!gStop)
        this_thread::sleep_for(chrono::seconds(1));
#else
    // TODO: Use SetConsoleCtrlHandler() to do something like the above, on Windows.
    cout << it("Press Enter to stop server: ");
    (void)getchar();
#endif

    cout << " Stopping server...\n";
    c4listener_free(_listener);
    _listener = nullptr;
}
