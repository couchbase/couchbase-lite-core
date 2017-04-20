//
//  LiteCoreServ.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4REST.h"
#include "FilePath.hh"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std;
using namespace litecore;


static const char* kCBLiteFileExtension = ".cblite2";

static uint16_t gPort = 59840;

static C4RESTListener *gListener;

static C4DatabaseConfig gConfig = {
    kC4DB_Bundled | kC4DB_SharedKeys
};


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


static string dbNameFromPath(const char *cpath) {
    FilePath path(cpath);
    string name = path.fileOrDirName();
    auto split = FilePath::splitExtension(name);
    if (split.second != kCBLiteFileExtension)
        fail("Database filename must end with '.cblite2'");
    return split.first;
}


static void startListener() {
    C4Error err;
    if (!gListener) {
        gListener = c4rest_start(gPort, &err);
        if (!gListener)
            fail("starting REST listener", err);
    }
}


static void shareDatabase(const char *path, string name) {
    startListener();
    C4Error err;
    auto db = c4db_open(c4str(path), &gConfig, &err);
    if (!db)
        fail("opening database", err);
    c4rest_shareDB(gListener, c4str(name), db);
    c4db_free(db);
}


static void shareDatabaseDir(const char *dirPath) {
    cerr << "Sharing all databases in " << dirPath << ": ";
    int n = 0;
    FilePath dir(dirPath, "");
    dir.forEachFile([=](const FilePath &file) mutable {
        if (file.extension() == kCBLiteFileExtension && file.existsAsDir()) {
            if (n++) cerr << ", ";
            auto dbPath = file.path().c_str();
            string name = dbNameFromPath(dbPath);
            cerr << name;
            shareDatabase(dbPath, name);
        }
    });
    cerr << "\n";
    if (n == 0)
        fail("No databases found");
}


int main(int argc, const char** argv) {
    try {
        auto restLog = c4log_getDomain("REST", true);
        c4log_setLevel(restLog, kC4LogInfo);

        for (int i = 1; i < argc; ++i) {
            auto arg = argv[i];
            if (arg[0] == '-') {
                // Flags:
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
                    gPort = (uint16_t) stoi(string(argv[i]));
                } else if (flag == "readonly") {
                    gConfig.flags |= kC4DB_ReadOnly;
                } else if (flag == "create") {
                    gConfig.flags |= kC4DB_Create;
                } else {
                    failMisuse("Unknown flag");
                }
            } else {
                // Paths:
                string name = dbNameFromPath(arg);
                cerr << "Sharing database '" << name << "' from " << arg << " ...\n";
                shareDatabase(arg, name);
            }
        }

        if (!gListener) {
            failMisuse("You must provide the path to at least one Couchbase Lite database to share.");
            exit(1);
        }
    } catch (const exception &x) {
        cerr << "\n";
        fail(x.what());
    }

    cerr << "LiteCoreServ is now listening at http://localhost:" << gPort << "/ ...\n";

    // Sleep to keep the process from exiting, while the server threads run:
    while(true)
        this_thread::sleep_for(chrono::hours(1000));
    return 0;
}
