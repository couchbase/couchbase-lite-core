//
// RESTListener.cc
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

#include "RESTListener.hh"
#include "c4.hh"
#include "c4Certificate.h"
#include "c4Database.h"
#include "c4Document+Fleece.h"
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
            addDBHandler(Method::GET,   "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleGetDatabase);
            addHandler  (Method::PUT,   "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleCreateDatabase);
            addDBHandler(Method::DELETE,"/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleDeleteDatabase);
            addDBHandler(Method::POST,  "/[^_][^/]*|/[^_][^/]*/",    &RESTListener::handleModifyDoc);

            // Database-level special handlers:
            addDBHandler(Method::GET,   "/[^_][^/]*/_all_docs",  &RESTListener::handleGetAllDocs);
            addDBHandler(Method::POST,  "/[^_][^/]*/_bulk_docs", &RESTListener::handleBulkDocs);

            // Document:
            addDBHandler(Method::GET,   "/[^_][^/]*/[^_].*",      &RESTListener::handleGetDoc);
            addDBHandler(Method::PUT,   "/[^_][^/]*/[^_].*",      &RESTListener::handleModifyDoc);
            addDBHandler(Method::DELETE,"/[^_][^/]*/[^_].*",      &RESTListener::handleModifyDoc);
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
        Retained<Cert> cert;
        try {
            cert = (Cert*)config->certificate;
        } catch (const error &) {
            error::_throw(error::InvalidParameter, "Can't parse certificate data");
        }

        Retained<PrivateKey> privateKey;
        switch (config->privateKeyRepresentation) {
            case kC4PrivateKeyFromKey:
                Assert(c4keypair_hasPrivateKey(config->key));
                privateKey = (PrivateKey*)config->key;
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
            tlsContext->setRootCerts((Cert*)tlsConfig->rootClientCerts);
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


    static bool returnError(C4Error* outError,
                            C4ErrorDomain domain, int code, const char *message =nullptr)
    {
        if (outError)
            *outError = c4error_make(domain, code, c4str(message));
        return false;
    }


    bool RESTListener::pathFromDatabaseName(const string &name, FilePath &path) {
        if (!_directory || !isValidDatabaseName(name))
            return false;
        string filename = name;
        replace(filename, '/', ':');
        path = (*_directory)[filename + kC4DatabaseFilenameExtension + "/"];
        return true;
    }


    bool RESTListener::openDatabase(std::string name,
                                    const FilePath &path,
                                    C4DatabaseFlags flags,
                                    C4Error *outError)
    {
        if (name.empty()) {
            name = databaseNameFromPath(path);
            if (name.empty())
                return returnError(outError, LiteCoreDomain, kC4ErrorInvalidParameter,
                                   "Invalid database name");
        }
        if (auto db = databaseNamed(name); db != nullptr)
            return returnError(outError, LiteCoreDomain, kC4ErrorConflict, "Database exists");
        C4DatabaseConfig2 config = {slice(path.dirName()), flags};
        c4::ref<C4Database> db = c4db_openNamed(slice(name), &config, outError);
        if (!db)
            return false;
        if (!registerDatabase(db, name)) {
            //FIX: If db didn't exist before the c4db_open call, should delete it
            return returnError(outError, LiteCoreDomain, kC4ErrorConflict, "Database exists");
        }
        return db;
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

    void RESTListener::addDBHandler(Method method, const char *uri, DBHandlerMethod handler) {
        _server->addHandler(method, uri, [this,handler](RequestResponse &rq) {
            c4::ref<C4Database> db = databaseFor(rq);
            if (db) {
                c4db_lock(db);
                try {
                    (this->*handler)(rq, db);
                } catch (...) {
                    c4db_unlock(db);
                    throw;
                }
                c4db_unlock(db);
            }
        });
    }

    
    c4::ref<C4Database> RESTListener::databaseFor(RequestResponse &rq) {
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

    

} }
