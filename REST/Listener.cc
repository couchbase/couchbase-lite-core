//
// Listener.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Listener.hh"
#include "DatabasePool.hh"
#include "c4Database.hh"
#include "c4ListenerInternal.hh"
#include "c4Private.h"  // for _c4db_setDatabaseTag
#include "Error.hh"

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;

namespace litecore::REST {


    Listener::Listener(const Config& config) : _config(config) {
        if ( !ListenerLog ) ListenerLog = c4log_getDomain("Listener", true);
    }

    string Listener::databaseNameFromPath(const FilePath& path) {
        string name  = path.fileOrDirName();
        auto   split = FilePath::splitExtension(name);
        if ( split.second != kC4DatabaseFilenameExtension )
            error::_throw(error::InvalidParameter, "Not a database path");
        name = split.first;

        // Make the name legal as a URI component in the REST API.
        // It shouldn't be empty, nor start with an underscore, nor contain control characters.
        if ( name.empty() ) name = "db";
        else if ( name[0] == '_' )
            name[0] = '-';
        for ( char& c : name ) {
            if ( iscntrl(c) || c == '/' ) c = '-';
        }
        return name;
    }

    bool Listener::isValidDatabaseName(const string& name) {
        if ( name.empty() || name.size() > 240 || name[0] == '_' ) return false;
        return std::all_of(name.begin(), name.end(), [](auto& c) { return !iscntrl(c); });
    }

    bool Listener::registerDatabase(C4Database* db, optional<string> name) {
        if ( !name ) {
            alloc_slice path(db->getPath());
            name = databaseNameFromPath(FilePath(string(path)));
        } else if ( !isValidDatabaseName(*name) ) {
            error::_throw(error::InvalidParameter, "Invalid name for sharing a database");
        }
        lock_guard<mutex> lock(_mutex);
        if ( _databases.contains(*name) ) return false;
        auto [i, added] = _databases.emplace(*name, new DatabasePool(db));
        i->second->onOpen([](C4Database* db) { _c4db_setDatabaseTag(db, DatabaseTag_RESTListener); });
        return true;
    }

    bool Listener::unregisterDatabase(const std::string& name) {
        lock_guard<mutex> lock(_mutex);
        auto              i = _databases.find(name);
        if ( i == _databases.end() ) return false;
        _databases.erase(i);

        auto j = _allowedCollections.find(name);
        if ( j != _allowedCollections.end() ) { _allowedCollections.erase(j); }

        return true;
    }

    bool Listener::unregisterDatabase(C4Database* db) {
        lock_guard<mutex> lock(_mutex);
        for ( auto i = _databases.begin(); i != _databases.end(); ++i ) {
            if ( i->second->sameAs(db) ) {
                _databases.erase(i);
                return true;
            }
        }
        return false;
    }

    bool Listener::registerCollection(const string& name, CollectionSpec collection) {
        lock_guard<mutex> lock(_mutex);
        auto              i = _databases.find(name);
        if ( i == _databases.end() ) return false;

        auto j = _allowedCollections.find(name);
        if ( j == _allowedCollections.end() ) {
            vector<CollectionSpec> collections({collection});
            _allowedCollections.emplace(name, collections);
        } else {
            j->second.push_back(collection);
        }

        return true;
    }

    bool Listener::unregisterCollection(const string& name, CollectionSpec collection) {
        lock_guard<mutex> lock(_mutex);
        auto              i = _allowedCollections.find(name);
        if ( i == _allowedCollections.end() ) return false;

        for ( auto j = i->second.begin(); j != i->second.end(); j++ ) {
            if ( *j == collection ) {
                i->second.erase(j);
                return true;
            }
        }

        return false;
    }

    DatabasePool* Listener::_databasePoolNamed(const std::string& name) const {
        auto i = _databases.find(name);
        if ( i == _databases.end() ) return nullptr;
        return i->second;
    }

    fleece::Retained<DatabasePool> Listener::databasePoolNamed(const std::string& name) const {
        lock_guard<mutex> lock(_mutex);
        return _databasePoolNamed(name);
    }

    optional<FilePath> Listener::pathOfDatabaseNamed(const std::string& name) {
        lock_guard<mutex> lock(_mutex);
        if ( auto pool = _databasePoolNamed(name) ) return pool->databasePath();
        else
            return nullopt;
    }

    BorrowedDatabase Listener::databaseNamed(const string& name, bool writeable) const {
        lock_guard<mutex> lock(_mutex);
        if ( auto pool = _databasePoolNamed(name) ) return writeable ? pool->borrowWriteable() : pool->borrow();
        else
            return {};
    }

    optional<string> Listener::nameOfDatabase(C4Database* db) const {
        lock_guard<mutex> lock(_mutex);
        for ( auto& [aName, aPool] : _databases )
            if ( aPool->sameAs(db) ) return aName;
        return nullopt;
    }

    vector<string> Listener::databaseNames() const {
        lock_guard     lock(_mutex);
        vector<string> names;
        names.reserve(_databases.size());
        for ( auto& d : _databases ) names.push_back(d.first);
        return names;
    }

    void Listener::closeDatabases() {
        lock_guard lock(_mutex);
        c4log(ListenerLog, kC4LogInfo, "Closing databases");
        for ( auto& d : _databases ) d.second->close();
        _databases.clear();
        _allowedCollections.clear();
    }

}  // namespace litecore::REST

#endif
