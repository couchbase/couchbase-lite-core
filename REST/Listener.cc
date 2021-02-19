//
// Listener.cc
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

#include "Listener.hh"
#include "c4Database.h"
#include "c4ListenerInternal.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include <vector>

using namespace std;
using namespace fleece;


namespace litecore { namespace REST {


    Listener::Listener(const Config &config)
    :_config(config)
    {
        if (!ListenerLog)
            ListenerLog = c4log_getDomain("Listener", true);
    }

    string Listener::databaseNameFromPath(const FilePath &path) {
        string name = path.fileOrDirName();
        auto split = FilePath::splitExtension(name);
        if (split.second != kC4DatabaseFilenameExtension)
            error::_throw(error::InvalidParameter, "Not a database path");
        name = split.first;

        // Make the name legal as a URI component in the REST API.
        // It shouldn't be empty, nor start with an underscore, nor contain control characters.
        if (name.size() == 0)
            name = "db";
        else if (name[0] == '_')
            name[0] = '-';
        for (char &c : name) {
            if (iscntrl(c) || c == '/')
                c = '-';
        }
        return name;
    }


    bool Listener::isValidDatabaseName(const string &name) {
        if (name.empty() || name.size() > 240 || name[0] == '_')
            return false;
        for (uint8_t c : name)
            if (iscntrl(c))
                return false;
        return true;
    }


    bool Listener::registerDatabase(C4Database* db, optional<string> name) {
        if (!name) {
            alloc_slice path(c4db_getPath(db));
            name = databaseNameFromPath(FilePath(string(path)));
        } else if (!isValidDatabaseName(*name)) {
            error::_throw(error::InvalidParameter, "Invalid name for sharing a database");
        }
        lock_guard<mutex> lock(_mutex);
        if (_databases.find(*name) != _databases.end())
            return false;
        _databases.emplace(*name, c4db_retain(db));
        return true;
    }


    bool Listener::unregisterDatabase(std::string name) {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return false;
        _databases.erase(i);
        return true;
    }


    bool Listener::unregisterDatabase(C4Database *db) {
        lock_guard<mutex> lock(_mutex);
        for (auto i = _databases.begin(); i != _databases.end(); ++i) {
            if (i->second == db) {
                _databases.erase(i);
                return true;
            }
        }
        return false;
    }


    c4::ref<C4Database> Listener::databaseNamed(const string &name) const {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return nullptr;
        // Retain the database to avoid a race condition if it gets unregistered while this
        // thread's handler is still using it.
        return c4::ref<C4Database>(c4db_retain(i->second));
    }


    optional<string> Listener::nameOfDatabase(C4Database *db) const {
        lock_guard<mutex> lock(_mutex);
        for (auto &[aName, aDB] : _databases)
            if (aDB == db)
                return aName;
        return nullopt;
    }


    vector<string> Listener::databaseNames() const {
        lock_guard<mutex> lock(_mutex);
        vector<string> names;
        for (auto &d : _databases)
            names.push_back(d.first);
        return names;
    }

} }
