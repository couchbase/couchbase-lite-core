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
#include "c4Certificate.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4Collection.hh"
#include "c4Private.h"
#include "Server.hh"
#include "TLSContext.hh"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"

using namespace std;
using namespace fleece;
using namespace litecore::crypto;


namespace litecore { namespace REST {
    using namespace net;

    //static constexpr const char* kKeepAliveTimeoutMS = "1000";
    //static constexpr const char* kMaxConnections = "8";

    static int kTaskExpirationTime = 10;


    string RESTListener::kServerName = "LiteCoreServ";


    string RESTListener::serverNameAndVersion() {
        alloc_slice version(c4_getVersion());
        return format("%s/%.*s", kServerName.c_str(), SPLAT(version));
    }


    RESTListener::RESTListener(const Config &config)
    :Listener(config)
    ,_directory(config.directory.buf ? new FilePath(slice(config.directory).asString(), "")
                                     : nullptr)
    ,_allowCreateDB(config.allowCreateDBs && _directory)
    ,_allowDeleteDB(config.allowDeleteDBs)
    {
        _server = new Server();
        _server->setExtraHeaders({{"Server", serverNameAndVersion()}});

        if (auto callback = config.httpAuthCallback; callback) {
            void *context = config.callbackContext;
            _server->setAuthenticator([=](slice authorizationHeader) {
                return callback((C4Listener*)this, authorizationHeader, context);
            });
        }

        if (config.apis & kC4RESTAPI) {
            // Root:
            addHandler(Method::GET, "/", &RESTListener::handleGetRoot);

            // Top-level special handlers:
            addHandler(Method::GET,     "/_all_dbs",         &RESTListener::handleGetAllDBs);
            addHandler(Method::GET,     "/_active_tasks",    &RESTListener::handleActiveTasks);
            addHandler(Method::POST,    "/_replicate",       &RESTListener::handleReplicate);

            // Database:
            addCollectionHandler(Method::GET,   "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleGetDatabase);
            addHandler  (Method::PUT,   "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleCreateDatabase);
            addDBHandler(Method::DELETE,"/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleDeleteDatabase);
            addCollectionHandler(Method::POST,  "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleModifyDoc);

            // Database-level special handlers:
            addCollectionHandler(Method::GET,   "/[^_][^/]*/_all_docs",  &RESTListener::handleGetAllDocs);
            addCollectionHandler(Method::POST,  "/[^_][^/]*/_bulk_docs", &RESTListener::handleBulkDocs);

            // Document:
            addCollectionHandler(Method::GET,   "/[^_][^/]*/[^_].*",      &RESTListener::handleGetDoc);
            addCollectionHandler(Method::PUT,   "/[^_][^/]*/[^_].*",      &RESTListener::handleModifyDoc);
            addCollectionHandler(Method::DELETE,"/[^_][^/]*/[^_].*",      &RESTListener::handleModifyDoc);
        }
        if (config.apis & kC4SyncAPI) {
            addDBHandler(Method::UPGRADE, "/[^_][^/]*/_blipsync", &RESTListener::handleSync);
        }

        _server->start(config.port,
                       config.networkInterface,
                       createTLSContext(config.tlsConfig).get());
    }


    RESTListener::~RESTListener() {
        stop();
    }


    void RESTListener::stop() {
        if (_server)
            _server->stop();
    }

    vector<Address> RESTListener::_addresses(C4Database *dbOrNull, C4ListenerAPIs api) const {
        optional<string> dbNameStr;
        slice dbName;
        if (dbOrNull) {
            dbNameStr = nameOfDatabase(dbOrNull);
            if (dbNameStr)
                dbName = *dbNameStr;
        }
        
        slice scheme;
        Assert((api == kC4RESTAPI || api == kC4SyncAPI));
        if(api == kC4RESTAPI) {
            scheme = _identity ? "https" : "http";
        } else if(api == kC4SyncAPI) {
            scheme = _identity ? "wss" : "ws";
        }

        uint16_t port = _server->port();
        vector<Address> addresses;
        for (auto &host : _server->addresses())
            addresses.emplace_back(scheme, host, port, dbName);
        return addresses;
    }

    vector<Address> RESTListener::addresses(C4Database *dbOrNull, C4ListenerAPIs api) const {
        if(api != kC4RESTAPI) {
            error::_throw(error::LiteCoreError::InvalidParameter,
                          "The listener is not running in the specified API mode.");
        }
        
        return _addresses(dbOrNull, api);
    }


#ifdef COUCHBASE_ENTERPRISE
    Retained<Identity> RESTListener::loadTLSIdentity(const C4TLSConfig *config) {
        if (!config)
            return nullptr;
        Retained<Cert> cert = config->certificate->assertSignedCert();
        Retained<PrivateKey> privateKey;
        switch (config->privateKeyRepresentation) {
            case kC4PrivateKeyFromKey:
                privateKey = config->key->getPrivateKey();
                break;
            case kC4PrivateKeyFromCert:
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
                privateKey = cert->loadPrivateKey();
                if (!privateKey)
                    error::_throw(error::CryptoError,
                                  "No persistent private key found matching certificate public key");
                break;
#else
                error::_throw(error::Unimplemented, "kC4PrivateKeyFromCert not implemented");
#endif
        }
        return new Identity(cert, privateKey);
    }
#endif // COUCHBASE_ENTERPRISE


    Retained<TLSContext> RESTListener::createTLSContext(const C4TLSConfig *tlsConfig) {
        if (!tlsConfig)
            return nullptr;
#ifdef COUCHBASE_ENTERPRISE
        _identity = loadTLSIdentity(tlsConfig);

        auto tlsContext = retained(new TLSContext(TLSContext::Server));
        tlsContext->setIdentity(_identity);
        if (tlsConfig->requireClientCerts)
            tlsContext->requirePeerCert(true);
        if (tlsConfig->rootClientCerts)
            tlsContext->setRootCerts(tlsConfig->rootClientCerts->assertSignedCert());
        if (auto callback = tlsConfig->certAuthCallback; callback) {
            auto context = tlsConfig->tlsCallbackContext;
            tlsContext->setCertAuthCallback([=](slice certData) {
                return callback((C4Listener*)this, certData, context);
            });
        }
        return tlsContext;
#else
        error::_throw(error::Unimplemented, "TLS server is an Enterprise Edition feature");
#endif
    }


    int RESTListener::connectionCount() {
        return _server->connectionCount();
    }


#pragma mark - REGISTERING DATABASES:


    static void replace(string &str, char oldChar, char newChar) {
        for (char &c : str)
            if (c == oldChar)
                c = newChar;
    }


    bool RESTListener::pathFromDatabaseName(const string &name, FilePath &path) {
        if (!_directory || !isValidDatabaseName(name))
            return false;
        string filename = name;
        replace(filename, '/', ':');
        path = (*_directory)[filename + kC4DatabaseFilenameExtension + "/"];
        return true;
    }


#pragma mark - TASKS:


    void RESTListener::Task::registerTask() {
        if (!_taskID) {
            time(&_timeStarted);
            _taskID = _listener->registerTask(this);
        }
    }


    void RESTListener::Task::unregisterTask() {
        if (_taskID) {
            _listener->unregisterTask(this);
            _taskID = 0;
        }
    }



    void RESTListener::Task::writeDescription(fleece::JSONEncoder &json) {
        json.writeKey("pid"_sl);
        json.writeUInt(_taskID);
        json.writeKey("started_on"_sl);
        json.writeUInt(_timeStarted);
    }


    unsigned RESTListener::registerTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.insert(task);
        return _nextTaskID++;
    }


    void RESTListener::unregisterTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.erase(task);
    }


    vector<Retained<RESTListener::Task>> RESTListener::tasks() {
        lock_guard<mutex> lock(_mutex);

        // Clean up old finished tasks:
        time_t now;
        time(&now);
        for (auto i = _tasks.begin(); i != _tasks.end(); ) {
            if ((*i)->finished() && (now - (*i)->timeUpdated()) >= kTaskExpirationTime)
                i = _tasks.erase(i);
            else
                ++i;
        }

        return vector<Retained<Task>>(_tasks.begin(), _tasks.end());
    }


#pragma mark - UTILITIES:


    void RESTListener::addHandler(Method method, const char *uri, HandlerMethod handler) {
        using namespace std::placeholders;
        _server->addHandler(method, uri, bind(handler, this, _1));
    }

    void RESTListener::addDBHandler(Method method, const char* uri, DBHandlerMethod handler) {
        _server->addHandler(method, uri, [this,handler](RequestResponse &rq) {
            Retained<C4Database> db = databaseFor(rq);
            if (db) {
                db->lockClientMutex();
                try {
                    (this->*handler)(rq, db);
                } catch (...) {
                    db->unlockClientMutex();
                    throw;
                }
                db->unlockClientMutex();
            }
        });
    }

    void RESTListener::addCollectionHandler(Method method, const char *uri, CollectionHandlerMethod handler) {
         _server->addHandler(method, uri, [this,handler](RequestResponse &rq) {
            Retained<C4Collection> coll = collectionFor(rq);

            if (coll) {
                coll->getDatabase()->lockClientMutex();
                try {
                    (this->*handler)(rq, coll);
                } catch (...) {
                    coll->getDatabase()->unlockClientMutex();
                    throw;
                }
                coll->getDatabase()->unlockClientMutex();
            }
        });
    }

    
    Retained<C4Database> RESTListener::databaseFor(RequestResponse &rq) {
        string dbName = rq.path(0);
        if (dbName.empty()) {
            rq.respondWithStatus(HTTPStatus::BadRequest);
            return nullptr;
        }
        auto db = databaseNamed(dbName);
        if (!db)
            rq.respondWithStatus(HTTPStatus::NotFound);
        return db;
    }

    inline vector<string> splitDbName(const string& input) {
        vector<string> retVal;
        stringstream data(input);
        string line;
        while(getline(data, line, '.')) {
            retVal.push_back(line);
        }

        return retVal;
    }

    Retained<C4Collection> RESTListener::collectionFor(RequestResponse &rq) {
        string dbName = rq.path(0);
        if (dbName.empty()) {
            rq.respondWithStatus(HTTPStatus::BadRequest);
            return nullptr;
        }

        auto dbParts = splitDbName(dbName);
        if(dbParts.size() != 1 && dbParts.size() != 3) {
            rq.respondWithStatus(HTTPStatus::BadRequest);
            return nullptr;
        }

        auto db = databaseNamed(dbParts[0]);
        if (!db) {
            rq.respondWithStatus(HTTPStatus::NotFound);
            return nullptr;
        }
         
        Retained<C4Collection> coll;
        if(dbParts.size() == 1) {
            coll = db->getDefaultCollection();
        } else {
            coll = db->getCollection({dbParts[1], dbParts[2]});
        }

        return coll;
    }

} }
