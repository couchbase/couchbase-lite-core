//
// Endpoint.cc
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
