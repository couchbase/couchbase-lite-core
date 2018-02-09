//
// LiteCoreServ.cc
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

#include "c4Listener.h"
#include "FilePath.hh"
#include "StringUtil.hh"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std;
using namespace litecore;


static C4Listener *gListener;

static C4ListenerConfig gListenerConfig;

static C4DatabaseConfig gDatabaseConfig = {
    kC4DB_SharedKeys
};

static string gDirectory;


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}

static inline C4Slice c4str(const string &s) {
    return {s.data(), s.size()};
}


static void usage() {
    cerr << "Usage: LiteCoreServ <options> <dbpath> ...  (serves each database)\n"
            "   or: LiteCoreServ <options> --dir <dir>   (serves all databases in <dir>)\n"
            "Options:\n"
            "       --port <n>         Listen on TCP port <n> (default is 59840)\n"
            "       --create           Create database(s) that don't exist\n"
            "       --readonly         Open database(s) read-only\n";
    if (c4listener_availableAPIs() & kC4SyncAPI)
        cerr <<
            "       --sync             Allow incoming sync/replication requests\n";
}


static void fail(const char *message) {
    cerr << "Error: " << message << "\n";
    exit(1);
}


static void fail(const char *what, C4Error err) {
    auto message = c4error_getMessage(err);
    cerr << "Error " << what << ": ";
    if (message.buf)
        cerr << to_string(message);
    cerr << "(" << err.domain << "/" << err.code << ")\n";
    exit(1);
}


static void failMisuse(const char *message ="Invalid parameters") {
    cerr << "Error: " << message << "\n";
    usage();
    exit(1);
}


static string databaseNameFromPath(const char *path) {
    C4StringResult nameSlice = c4db_URINameFromPath(c4str(path));
    if (!nameSlice.buf)
        return string();
    string name((char*)nameSlice.buf, nameSlice.size);
    c4slice_free(nameSlice);
    return name;
}


static void startListener() {
    if (!gListener) {
        C4Error err;
        gListener = c4listener_start(&gListenerConfig, &err);
        if (!gListener)
            fail("starting REST listener", err);
    }
}


static void shareDatabase(const char *path, string name) {
    startListener();
    C4Error err;
    auto db = c4db_open(c4str(path), &gDatabaseConfig, &err);
    if (!db)
        fail("opening database", err);
    c4listener_shareDB(gListener, c4str(name), db);
    c4db_free(db);
}


static void shareDatabaseDir(const char *dirPath) {
    gDirectory = dirPath;
    gListenerConfig.directory = c4str(gDirectory);
    startListener();
    cerr << "Sharing all databases in " << dirPath << ": ";
    int n = 0;
    FilePath dir(dirPath, "");
    dir.forEachFile([=](const FilePath &file) mutable {
        if (file.isDir() && file.extension() == kC4DatabaseFilenameExtension) {
            auto path = file.path();
            string name = databaseNameFromPath(path.c_str());
            if (!name.empty()) {
                if (n++) cerr << ", ";
                cerr << name;
                shareDatabase(path.c_str(), name);
            }
        }
    });
    cerr << "\n";
}


int main(int argc, const char** argv) {
    // Default configuration (everything else is false/zero):
    gListenerConfig.port = 59840;
    gListenerConfig.apis = kC4RESTAPI;
    gListenerConfig.allowCreateDBs = gListenerConfig.allowDeleteDBs = true;
    gListenerConfig.allowPush = gListenerConfig.allowPull = true;

    try {
        auto restLog = c4log_getDomain("REST", true);
        c4log_setLevel(restLog, kC4LogInfo);

        for (int i = 1; i < argc; ++i) {
            auto arg = argv[i];
            if (arg[0] == '-') {
                // Flags:
                if (gListener)
                    fail("Flags can't go after a database path or directory");

                while (arg[0] == '-')
                    ++arg;
                string flag = arg;
                if (flag == "help") {
                    usage();
                    exit(0);
                } else if (flag == "dir") {
                    if (++i >= argc)
                        failMisuse();
                    shareDatabaseDir(argv[i]);
                } else if (flag == "port") {
                    if (++i >= argc)
                        failMisuse();
                    gListenerConfig.port = (uint16_t) stoi(string(argv[i]));
                } else if (flag == "readonly") {
                    gDatabaseConfig.flags |= kC4DB_ReadOnly;
                    gListenerConfig.allowCreateDBs = gListenerConfig.allowDeleteDBs = false;
                } else if (flag == "create") {
                    gDatabaseConfig.flags |= kC4DB_Create;
                } else if (flag == "sync") {
                    gListenerConfig.apis |= kC4SyncAPI;
                } else {
                    failMisuse("Unknown flag");
                }
            } else {
                // Paths:
                string name = databaseNameFromPath(arg);
                if (name.empty())
                    fail("Invalid database name");
                cerr << "Sharing database '" << name << "' from " << arg << " ...\n";
                shareDatabase(arg, name);
            }
        }

        if (!gListener) {
            failMisuse("Please specify a database directory or at least one database path");
            exit(1);
        }
    } catch (const exception &x) {
        cerr << "\n";
        fail(x.what());
    }

    cerr << "LiteCoreServ is now listening at http://localhost:" << gListenerConfig.port << "/ ...\n";

    // Sleep to keep the process from exiting, while the server threads run:
    while(true)
        this_thread::sleep_for(chrono::hours(1000));
    return 0;
}
