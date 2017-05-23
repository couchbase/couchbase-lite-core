//
//  Listener.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/22/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Listener.hh"
#include "c4ListenerInternal.hh"
#include "StringUtil.hh"
#include <vector>

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace REST {


    Listener::Listener() {
        if (!RESTLog)
            RESTLog = c4log_getDomain("REST", true);
    }

    string Listener::databaseNameFromPath(const FilePath &path) {
        string name = path.fileOrDirName();
        auto split = FilePath::splitExtension(name);
        if (split.second != kC4DatabaseFilenameExtension)
            return string();
        name = split.first;
        replace(name, ':', '/');
        if (!isValidDatabaseName(name))
            return string();
        return name;
    }


    bool Listener::isValidDatabaseName(const string &name) {
        if (name.size() == 0 || name.size() > 240 || name[0] == '_')
            return false;
        for (uint8_t c : name)
            if (iscntrl(c))
                return false;
        return true;
    }


    bool Listener::registerDatabase(string name, C4Database *db) {
        if (!isValidDatabaseName(name))
            return false;
        lock_guard<mutex> lock(_mutex);
        if (_databases.find(name) != _databases.end())
            return false;
        _databases.emplace(name, c4db_retain(db));
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


    c4::ref<C4Database> Listener::databaseNamed(const string &name) {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return nullptr;
        // Retain the database to avoid a race condition if it gets unregistered while this
        // thread's handler is still using it.
        return c4::ref<C4Database>(c4db_retain(i->second));
    }


    vector<string> Listener::databaseNames() {
        lock_guard<mutex> lock(_mutex);
        vector<string> names;
        for (auto &d : _databases)
            names.push_back(d.first);
        return names;
    }

} }
