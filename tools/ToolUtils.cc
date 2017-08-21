//
//  ToolUtils.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ToolUtils.hh"


int gVerbose {0};
bool gFailOnError {false};


void errorOccurred(const string &what) {
    cerr << "Error " << what << "\n";
    if (gFailOnError)
        exit(1);
}


void errorOccurred(const string &what, C4Error err) {
    alloc_slice message = c4error_getMessage(err);
    cerr << "Error " << what << ": ";
    if (message.buf)
        cerr << to_string(message) << ' ';
    cerr << "(" << err.domain << "/" << err.code << ")\n";
    if (gFailOnError)
        exit(1);
}


[[noreturn]] void fail(const string &message) {
    errorOccurred(message);
    exit(1);
}


[[noreturn]] void fail(const string &what, C4Error err) {
    errorOccurred(what, err);
    exit(1);
}


[[noreturn]] void failMisuse(const char *message) {
    cerr << "Error: " << message << "\n";
    usage();
    exit(1);
}
