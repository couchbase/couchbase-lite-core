//
//  LiteCoreServ.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4REST.h"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std;


static uint16_t gPort = 59840;

static C4RESTListener *gListener;

static C4DatabaseConfig gConfig = {
    kC4DB_Bundled | kC4DB_SharedKeys
};


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}


static void fail(const char *what, C4Error err) {
    auto message = c4error_getMessage(err);
    cerr << "Error " << what << ": ";
    if (message.buf)
        cerr << to_string(message);
    cerr << "(" << err.domain << "/" << err.code << ")\n";
    exit(1);
}


static C4String dbNameFromPath(const char *path) {
    // Find the end of the filename:
    const char *end = path + strlen(path);
    while (end > path && end[-1] == '/')
        --end;
    for (const char *extn = end; extn > path && extn[-1] != '/'; --extn) {
        if (extn[-1] == '.') {
            end = extn - 1;
            break;
        }
    }
    const char *filename = end;
    while (filename > path && filename[-1] != '/')
        --filename;

    return C4String{filename, (size_t)(end - filename)};
}


static void shareDatabase(const char *path, C4String name) {
    C4Error err;
    if (!gListener) {
        gListener = c4rest_start(gPort, &err);
        if (!gListener)
            fail("starting REST listener", err);
        cerr << "LiteCoreServ is now listening at http://localhost:" << gPort << "/ ...\n";
    }

    cerr << "Sharing database '" << to_string(name) << "' from " << path << " ...\n";
    auto db = c4db_open(c4str(path), &gConfig, &err);
    if (!db)
        fail("opening database", err);
    c4rest_shareDB(gListener, name, db);
    c4db_free(db);
}


int main(int argc, const char** argv) {
    for (int i = 1; i < argc; ++i) {
        auto arg = argv[i];
        if (arg[0] == '-') {
            // Flags:
            while (arg[0] == '-')
                ++arg;
            string flag = arg;
            if (flag == "port") {
                gPort = (uint16_t) stoi(string(argv[++i]));
            } else {
                cerr << "Error: Unknown flag " << arg << "\n";
                exit(1);
            }
        } else {
            // Paths:
            shareDatabase(arg, dbNameFromPath(arg));
        }
    }

    if (!gListener) {
        cerr << "Error: You must provide the path to at least one Couchbase Lite database to share.\n";
        exit(1);
    }

    // Sleep to keep the process from exiting, while the server threads run:
    while(true)
        this_thread::sleep_for(chrono::hours(1000));
    return 0;
}
