//
//  ToolUtils.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "StringUtil.hh"
#include "FleeceCpp.hh"
#include "slice.hh"
#include <iostream>
#include <string>


using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;


extern int gVerbose;

extern bool gFailOnError;


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}

static inline C4Slice c4str(const string &s) {
    return {s.data(), s.size()};
}


void usage();

void errorOccurred(const string &what);
void errorOccurred(const string &what, C4Error err);

[[noreturn]] void fail(const string &message);
[[noreturn]] void fail(const string &what, C4Error err);

[[noreturn]] void failMisuse(const char *message ="Invalid parameters");

