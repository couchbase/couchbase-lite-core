//
// RESTListener.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RESTListener.hh"
#include "DatabasePool.hh"
#include "c4Certificate.hh"
#include "c4Database.hh"
#include "c4ListenerInternal.hh"
#include "Server.hh"
#include "TLSContext.hh"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"

using namespace std;
using namespace fleece;
using namespace litecore::crypto;

namespace litecore::REST {
    using namespace net;

    //static constexpr const char* kKeepAliveTimeoutMS = "1000";
    //static constexpr const char* kMaxConnections = "8";

    static int kTaskExpirationTime = 10;

    RESTListener::RESTListener(const Config& config)
        : Listener(config)
        , _directory(config.directory.buf ? new FilePath(slice(config.directory).asString(), "") : nullptr)
        , _allowCreateDB(config.allowCreateDBs && _directory)
        , _allowDeleteDB(config.allowDeleteDBs)
        , _allowCreateCollection(config.allowCreateCollections)
        , _allowDeleteCollection(config.allowDeleteCollections)
        , _allowQueries(config.allowQueries) {
        _server        = new Server();
        _serverName    = config.serverName ? slice(config.serverName) : "CouchbaseLite"_sl;
        _serverVersion = config.serverVersion ? slice(config.serverVersion) : alloc_slice(c4_getVersion());
        _server->setExtraHeaders({{"Server", _serverName + "/" + _serverVersion}});

        if ( auto callback = config.httpAuthCallback; callback ) {
            void* context = config.callbackContext;
            _server->setAuthenticator([this, callback, context](slice authorizationHeader) {
                return callback((C4Listener*)this, authorizationHeader, context);
            });
        }

        const APIVersion v1{1, 0};

        if ( config.apis & kC4RESTAPI ) {
            // Root:
            addHandler(Method::GET, "/", v1, &RESTListener::handleGetRoot);

            // Top-level special handlers:
            addHandler(Method::GET, "/_all_dbs", v1, &RESTListener::handleGetAllDBs);
            addHandler(Method::GET, "/_active_tasks", v1, &RESTListener::handleActiveTasks);
            addHandler(Method::POST, "/_replicate", v1, &RESTListener::handleReplicate);

            // Database:
            addCollectionHandler(Method::GET, "/*", false, v1, &RESTListener::handleGetDatabase);
            addHandler(Method::PUT, "/*", v1, &RESTListener::handleCreateDatabase);
            addHandler(Method::DELETE, "/*", v1, &RESTListener::handleDeleteDatabase);
            addCollectionHandler(Method::POST, "/*", true, v1, &RESTListener::handleModifyDoc);

            // Database-level special handlers:
            addCollectionHandler(Method::GET, "/*/_all_docs", false, v1, &RESTListener::handleGetAllDocs);
            addCollectionHandler(Method::POST, "/*/_bulk_docs", true, v1, &RESTListener::handleBulkDocs);
            addCollectionHandler(Method::GET, "/*/_changes", false, v1, &RESTListener::handleChanges);
            addCollectionHandler(Method::GET, "/*/_function/*", false, v1, &RESTListener::handleFunction);
            addCollectionHandler(Method::POST, "/*/_query", false, v1, &RESTListener::handleQuery);

            // Document:
            addCollectionHandler(Method::GET, "/*/*", false, v1, &RESTListener::handleGetDoc);
            addCollectionHandler(Method::PUT, "/*/*", true, v1, &RESTListener::handleModifyDoc);
            addCollectionHandler(Method::DELETE, "/*/*", true, v1, &RESTListener::handleModifyDoc);
        }
        if ( config.apis & kC4SyncAPI ) {
            addDBHandler(Method::UPGRADE, "/*/_blipsync", false, v1, &RESTListener::handleSync);
        }

        _server->start(config.port, config.networkInterface, createTLSContext(config.tlsConfig).get());
    }

    RESTListener::~RESTListener() { stop(); }

    void RESTListener::stop() {
        if ( _server ) _server->stop();
    }

    vector<Address> RESTListener::_addresses(C4Database* dbOrNull, C4ListenerAPIs api) const {
        optional<string> dbNameStr;
        slice            dbName;
        if ( dbOrNull ) {
            dbNameStr = nameOfDatabase(dbOrNull);
            if ( dbNameStr ) dbName = *dbNameStr;
        }

        slice scheme;
        Assert((api == kC4RESTAPI || api == kC4SyncAPI));
        if ( api == kC4RESTAPI ) {
            scheme = _identity ? "https" : "http";
        } else if ( api == kC4SyncAPI ) {
            scheme = _identity ? "wss" : "ws";
        }

        uint16_t        port = _server->port();
        vector<Address> addresses;
        for ( auto& host : _server->addresses() ) addresses.emplace_back(scheme, host, port, dbName);
        return addresses;
    }

    vector<Address> RESTListener::addresses(C4Database* dbOrNull, C4ListenerAPIs api) const {
        if ( api != kC4RESTAPI ) {
            error::_throw(error::LiteCoreError::InvalidParameter,
                          "The listener is not running in the specified API mode.");
        }

        return _addresses(dbOrNull, api);
    }


#ifdef COUCHBASE_ENTERPRISE
    Retained<Identity> RESTListener::loadTLSIdentity(const C4TLSConfig* config) {
        if ( !config ) return nullptr;
        Retained<Cert>       cert = config->certificate->assertSignedCert();
        Retained<PrivateKey> privateKey;
        switch ( config->privateKeyRepresentation ) {
            case kC4PrivateKeyFromKey:
                privateKey = config->key->getPrivateKey();
                break;
            case kC4PrivateKeyFromCert:
#    ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
                privateKey = cert->loadPrivateKey();
                if ( !privateKey )
                    error::_throw(error::CryptoError,
                                  "No persistent private key found matching certificate public key");
                break;
#    else
                error::_throw(error::Unimplemented, "kC4PrivateKeyFromCert not implemented");
#    endif
        }
        return new Identity(cert, privateKey);
    }
#endif  // COUCHBASE_ENTERPRISE


    Retained<TLSContext> RESTListener::createTLSContext(const C4TLSConfig* tlsConfig) {
        if ( !tlsConfig ) return nullptr;
#ifdef COUCHBASE_ENTERPRISE
        _identity = loadTLSIdentity(tlsConfig);

        auto tlsContext = retained(new TLSContext(TLSContext::Server));
        tlsContext->setIdentity(_identity);
        if ( tlsConfig->requireClientCerts ) tlsContext->requirePeerCert(true);
        if ( tlsConfig->rootClientCerts ) tlsContext->setRootCerts(tlsConfig->rootClientCerts->assertSignedCert());
        if ( auto callback = tlsConfig->certAuthCallback; callback ) {
            auto context = tlsConfig->tlsCallbackContext;
            tlsContext->setCertAuthCallback([callback, this, context](slice certData) {
                return callback((C4Listener*)this, certData, context);
            });
        }
        return tlsContext;
#else
        error::_throw(error::Unimplemented, "TLS server is an Enterprise Edition feature");
#endif
    }

    int RESTListener::connectionCount() { return _server->connectionCount(); }

#pragma mark - REGISTERING DATABASES:

    static void replace(string& str, char oldChar, char newChar) {
        for ( char& c : str )
            if ( c == oldChar ) c = newChar;
    }

    bool RESTListener::pathFromDatabaseName(const string& name, FilePath& path) {
        if ( !_directory || !isValidDatabaseName(name) ) return false;
        string filename = name;
        replace(filename, '/', ':');
        path = (*_directory)[filename + kC4DatabaseFilenameExtension + "/"];
        return true;
    }

#pragma mark - TASKS:

    void RESTListener::Task::registerTask() {
        if ( !_taskID ) {
            _timeStarted = ::time(nullptr);
            _taskID      = _listener->registerTask(this);
        }
    }

    void RESTListener::Task::unregisterTask() {
        if ( _taskID ) {
            _taskID = 0;
            _listener->unregisterTask(this);
        }
    }

    void RESTListener::Task::bumpTimeUpdated() { _timeUpdated = ::time(nullptr); }

    void RESTListener::Task::writeDescription(fleece::JSONEncoder& json) {
        json.writeFormatted("pid: %u, started_on: %lu", _taskID, _timeStarted.load());
    }

    unsigned RESTListener::registerTask(Task* task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.insert(task);
        return _nextTaskID++;
    }

    void RESTListener::unregisterTask(Task* task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.erase(task);
    }

    vector<Retained<RESTListener::Task>> RESTListener::tasks() {
        lock_guard<mutex>                    lock(_mutex);
        vector<Retained<RESTListener::Task>> result{_tasks.begin(), _tasks.end()};
        // Clean up old finished tasks:
        time_t now;
        time(&now);
        for ( auto i = _tasks.begin(); i != _tasks.end(); ) {
            if ( (*i)->finished() && (now - (*i)->timeUpdated()) >= kTaskExpirationTime ) i = _tasks.erase(i);
            else
                ++i;
        }
        return result;
    }

#pragma mark - UTILITIES:

    void RESTListener::addHandler(Method method, const char* uri, APIVersion vers, HandlerMethod handler) {
        using namespace std::placeholders;
        _server->addHandler(method, uri, vers, bind(handler, this, _1));
    }

    void RESTListener::addDBHandler(Method method, const char* uri, bool writeable, APIVersion vers,
                                    DBHandlerMethod handler) {
        _server->addHandler(method, uri, vers, [=, this](RequestResponse& rq) {
            BorrowedDatabase db = getDatabase(rq, rq.path(0), writeable);
            if ( db ) (this->*handler)(rq, db);
        });
    }

    void RESTListener::addCollectionHandler(Method method, const char* uri, bool writeable, APIVersion vers,
                                            CollectionHandlerMethod handler) {
        _server->addHandler(method, uri, vers, [=, this](RequestResponse& rq) {
            auto [db, collection] = collectionFor(rq, writeable);
            if ( db ) (this->*handler)(rq, collection);
        });
    }

    pair<string, C4CollectionSpec> RESTListener::parseKeySpace(slice keySpace) {
        slice_istream in(keySpace);
        slice         dbName = in.readToDelimiter(".");
        if ( !dbName ) return {string(keySpace), {}};
        C4CollectionSpec spec = {};
        spec.name             = in.readToDelimiterOrEnd(".");
        if ( in.size > 0 ) {
            spec.scope = spec.name;
            spec.name  = in;
        }
        return {string(dbName), spec};
    }

    bool RESTListener::collectionGiven(RequestResponse& rq) { return !!slice(rq.path(0)).findByte('.'); }

    BorrowedDatabase RESTListener::getDatabase(RequestResponse& rq, const string& dbName, bool writeable) {
        BorrowedDatabase db = databaseNamed(dbName, writeable);
        if ( !db ) {
            if ( isValidDatabaseName(dbName) ) rq.respondWithStatus(HTTPStatus::NotFound, "No such database");
            else
                rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid database name");
        }
        return db;
    }

    // returning the retained db is necessary because retaining a collection does not retain its db!
    pair<BorrowedDatabase, C4Collection*> RESTListener::collectionFor(RequestResponse& rq, bool writeable) {
        string keySpace     = rq.path(0);
        auto [dbName, spec] = parseKeySpace(keySpace);
        BorrowedDatabase db = getDatabase(rq, dbName, writeable);
        if ( !db ) return {};
        if ( !spec.name.buf ) spec.name = kC4DefaultCollectionName;
        C4Collection* collection;
        try {
            collection = db->getCollection(spec);
        } catch ( const std::exception& ) {
            rq.respondWithError(C4Error::fromCurrentException());
            return {};
        }
        if ( !collection ) {
            rq.respondWithStatus(HTTPStatus::NotFound, "No such collection");
            return {};
        }
        return {std::move(db), collection};
    }

}  // namespace litecore::REST
