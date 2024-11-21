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
#include "slice_stream.hh"

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;

namespace litecore::REST {


    Listener::Listener(const Config& config) : _config(config) {
        if ( !ListenerLog ) ListenerLog = c4log_getDomain("Listener", true);
    }

    string Listener::makeKeyspace(string_view dbName, C4CollectionSpec const& coll) {
        string keyspace(dbName);
        bool   hasScope = (coll.scope && coll.scope != kC4DefaultScopeID);
        if ( hasScope ) {
            keyspace += ".";
            keyspace += slice(coll.scope);
        }
        if ( hasScope || (coll.name && coll.name != kC4DefaultCollectionName) ) {
            keyspace += ".";
            keyspace += slice(coll.name ? coll.name : kC4DefaultCollectionName);
        }
        return keyspace;
    }

    pair<string_view, C4CollectionSpec> Listener::parseKeyspace(slice keyspace) {
        slice_istream in(keyspace);
        slice         dbName = in.readToDelimiter(".");
        if ( !dbName ) return {keyspace, kC4DefaultCollectionSpec};
        C4CollectionSpec spec = {};
        spec.name             = in.readToDelimiterOrEnd(".");
        if ( in.size > 0 ) {
            spec.scope = spec.name;
            spec.name  = in;
        } else {
            spec.scope = kC4DefaultScopeID;
        }
        return {dbName, spec};
    }

    static bool isCharValidInDBName(char c) {
        // '.' is a keyspace collection delimiter, '/' is a path separator.
        return c >= ' ' && c < 0x7F && c != '.' && c != '/';
    }

    string Listener::databaseNameFromPath(const FilePath& path) {
        string name  = path.fileOrDirName();
        auto   split = FilePath::splitExtension(name);
        if ( split.second != kC4DatabaseFilenameExtension )
            error::_throw(error::InvalidParameter, "Not a database path");
        name = split.first;

        // Make the name legal as a URI component in the REST API.
        // It shouldn't be empty, nor start with an underscore, nor contain reserved characters.
        if ( name.empty() ) name = "db";
        else if ( name[0] == '_' )
            name[0] = '-';
        for ( char& c : name )
            if ( !isCharValidInDBName(c) ) c = '-';
        return name;
    }

    bool Listener::isValidDatabaseName(const string& name) {
        if ( name.empty() || name.size() > 240 || name[0] == '_' ) return false;
        return std::all_of(name.begin(), name.end(), isCharValidInDBName);
    }

    bool Listener::registerDatabase(C4Database* db, optional<string> name, C4ListenerDatabaseConfig const* dbConfig) {
        if ( !name ) {
            alloc_slice path(db->getPath());
            name = databaseNameFromPath(FilePath(string(path)));
        } else if ( !isValidDatabaseName(*name) ) {
            error::_throw(error::InvalidParameter, "Invalid name for sharing a database");
        }
        lock_guard<mutex> lock(_mutex);
        if ( _databases.contains(*name) ) return false;

        DBShare share{.pool = new DatabasePool(db), .keySpaces = {makeKeyspace(*name, kC4DefaultCollectionSpec)}};
        share.pool->onOpen([](C4Database* db) { _c4db_setDatabaseTag(db, DatabaseTag_RESTListener); });
        if ( dbConfig ) {
            share.config = *dbConfig;
        } else {
            share.config = {.allowPull       = _config.allowPull,
                            .allowPush       = _config.allowPush,
                            .enableDeltaSync = _config.enableDeltaSync};
        }
        _databases.emplace(*name, std::move(share));
        return true;
    }

    bool Listener::unregisterDatabase(const std::string& name) {
        lock_guard<mutex> lock(_mutex);
        auto              i = _databases.find(name);
        if ( i == _databases.end() ) return false;
        _databases.erase(i);
        return true;
    }

    bool Listener::unregisterDatabase(C4Database* db) {
        lock_guard<mutex> lock(_mutex);
        for ( auto i = _databases.begin(); i != _databases.end(); ++i ) {
            if ( i->second.pool->sameAs(db) ) {
                _databases.erase(i);
                return true;
            }
        }
        return false;
    }

    Listener::DBShare* Listener::_getShare(std::string const& name) {
        auto i = _databases.find(name);
        return i != _databases.end() ? &i->second : nullptr;
    }

    Listener::DBShare const* Listener::_getShare(std::string const& name) const {
        return const_cast<Listener*>(this)->_getShare(name);
    }

    optional<Listener::DBShare> Listener::getShare(std::string const& name) const {
        lock_guard<mutex> lock(_mutex);
        optional<DBShare> result;
        if ( auto share = _getShare(name) ) result.emplace(*share);
        return result;
    }

    bool Listener::registerCollection(const string& name, CollectionSpec collection) {
        lock_guard<mutex> lock(_mutex);
        auto              share = _getShare(name);
        if ( !share ) return false;
        share->keySpaces.insert(makeKeyspace(name, collection));
        return true;
    }

    bool Listener::unregisterCollection(const string& name, CollectionSpec collection) {
        lock_guard<mutex> lock(_mutex);
        auto              share = _getShare(name);
        return share && share->keySpaces.erase(makeKeyspace(name, collection)) > 0;
    }

    BorrowedDatabase Listener::borrowDatabaseNamed(const string& name, bool writeable) const {
        lock_guard<mutex> lock(_mutex);
        if ( auto share = _getShare(name) ) return writeable ? share->pool->borrowWriteable() : share->pool->borrow();
        else
            return {};
    }

    BorrowedCollection Listener::borrowCollection(const string& keyspace, bool writeable) const {
        auto [dbName, spec] = parseKeyspace(keyspace);
        lock_guard<mutex> lock(_mutex);
        auto              share = _getShare(string(dbName));
        if ( !share || !share->keySpaces.contains(keyspace) ) return {};
        return BorrowedCollection(writeable ? share->pool->borrowWriteable() : share->pool->borrow(), spec);
    }

    optional<string> Listener::nameOfDatabase(C4Database* db) const {
        lock_guard<mutex> lock(_mutex);
        for ( auto& [aName, share] : _databases )
            if ( share.pool->sameAs(db) ) return aName;
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
        for ( auto& d : _databases ) d.second.pool->close();
        _databases.clear();
    }

}  // namespace litecore::REST

#endif
