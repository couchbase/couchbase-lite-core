//
//  Endpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Endpoint.hh"
#include "DBEndpoint.hh"
#include "JSONEndpoint.hh"
#include "DirEndpoint.hh"


Endpoint* Endpoint::create(const string &str) {
    if (hasSuffix(str, kC4DatabaseFilenameExtension)) {
        return new DbEndpoint(str);
    } else if (hasSuffix(str, ".json")) {
        return new JSONEndpoint(str);
    } else if (hasPrefix(str, "blip:") || hasPrefix(str, "blips:")) {
        fail("Sorry, remote databases are not yet implemented");
        //return new RemoteEndpoint(str);
    } else if (hasSuffix(str, FilePath::kSeparator)) {
        return new DirectoryEndpoint(str);
    } else {
        if (FilePath(str).existsAsDir() || str.find('.') == string::npos)
            cerr << "HINT: If you are trying to copy to/from a directory of JSON files, append a '/' to the path.\n";
        return nullptr;
    }
}
