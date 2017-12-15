//
//  Endpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Endpoint.hh"
#include "DBEndpoint.hh"
#include "RemoteEndpoint.hh"
#include "JSONEndpoint.hh"
#include "DirEndpoint.hh"
#include "Error.hh"


Endpoint* Endpoint::create(const string &str) {
    if (hasPrefix(str, "blip://") || hasPrefix(str, "blips://") ||
            hasPrefix(str, "ws://") || hasPrefix(str, "wss://")) {
        return new RemoteEndpoint(str);
    } else if (hasSuffix(str, kC4DatabaseFilenameExtension)) {
        return new DbEndpoint(str);
    } else if (hasSuffix(str, ".json")) {
        return new JSONEndpoint(str);
    } else if (hasSuffix(str, FilePath::kSeparator)) {
        return new DirectoryEndpoint(str);
    } else {
        if (str.find("://") != string::npos)
            cerr << "HINT: Replication URLs must use the 'blip:' or 'blips:' schemes.\n";
        else if (FilePath(str).existsAsDir() || str.find('.') == string::npos)
            cerr << "HINT: If you are trying to copy to/from a directory of JSON files, append a '/' to the path.\n";
        return nullptr;
    }
}


Endpoint* Endpoint::create(C4Database *db) {
    Assert(db != nullptr);
    return new DbEndpoint(db);
}
