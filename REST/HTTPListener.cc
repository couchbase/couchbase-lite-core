//
// HTTPListener.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "HTTPListener.hh"
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

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace litecore::crypto;

namespace litecore::REST {
    using namespace net;

    static int kTaskExpirationTime = 10;

    HTTPListener::HTTPListener(const Config& config) : Listener(config) {
        _server        = new Server();
        _serverName    = config.serverName ? slice(config.serverName) : "CouchbaseLite"_sl;
        _serverVersion = config.serverVersion ? slice(config.serverVersion) : alloc_slice(c4_getVersion());
        _server->setExtraHeaders({{"Server", _serverName + "/" + _serverVersion}});

        if ( auto callback = config.httpAuthCallback ) {
            auto context = config.callbackContext;
            _server->setAuthenticator([=, this](slice authHeader) -> bool {
                return _delegate && callback(_delegate, authHeader, context);
            });
        }

        _server->start(config.port, config.networkInterface, createTLSContext(config.tlsConfig).get());
    }

    HTTPListener::~HTTPListener() { stop(); }

    void HTTPListener::stop() {
        if ( _server ) _server->stop();
        stopTasks();
        closeDatabases();
    }

    vector<Address> HTTPListener::addresses(C4Database* dbOrNull, bool webSocketScheme) const {
        optional<string> dbNameStr;
        slice            dbName;
        if ( dbOrNull ) {
            dbNameStr = nameOfDatabase(dbOrNull);
            if ( dbNameStr ) dbName = *dbNameStr;
        }

        string scheme = webSocketScheme ? "ws" : "http";
        if ( _identity ) scheme += 's';

        uint16_t        port = _server->port();
        vector<Address> addresses;
        for ( auto& host : _server->addresses() ) addresses.emplace_back(scheme, host, port, dbName);
        return addresses;
    }

    Retained<Identity> HTTPListener::loadTLSIdentity(const C4TLSConfig* config) {
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

    Retained<TLSContext> HTTPListener::createTLSContext(const C4TLSConfig* tlsConfig) {
        if ( !tlsConfig ) return nullptr;
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
    }

    int HTTPListener::connectionCount() { return _server->connectionCount(); }

#    pragma mark - TASKS:

    void HTTPListener::Task::registerTask() {
        if ( !_taskID ) {
            _timeStarted = ::time(nullptr);
            _taskID      = _listener->registerTask(this);
        }
    }

    void HTTPListener::Task::unregisterTask() {
        if ( _taskID ) {
            _taskID = 0;
            _listener->unregisterTask(this);
        }
    }

    void HTTPListener::Task::bumpTimeUpdated() { _timeUpdated = ::time(nullptr); }

    void HTTPListener::Task::writeDescription(fleece::JSONEncoder& json) {
        json.writeFormatted("pid: %u, started_on: %lu", _taskID, _timeStarted.load());
    }

    unsigned HTTPListener::registerTask(Task* task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.insert(task);
        return _nextTaskID++;
    }

    void HTTPListener::unregisterTask(Task* task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.erase(task);
        _tasksCondition.notify_all();
    }

    vector<Retained<HTTPListener::Task>> HTTPListener::tasks() {
        lock_guard<mutex>      lock(_mutex);
        vector<Retained<Task>> result{_tasks.begin(), _tasks.end()};
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

    void HTTPListener::stopTasks() {
        auto allTasks = tasks();
        if ( !allTasks.empty() ) {
            for ( auto& task : allTasks ) {
                if ( !task->finished() ) task->stop();
            }
            unique_lock lock(_mutex);
            _tasksCondition.wait(lock, [this] { return _tasks.empty(); });
        }
    }

#    pragma mark - UTILITIES:

    void HTTPListener::addHandler(Method method, const char* uri, APIVersion vers, Server::Handler handler) {
        _server->addHandler(method, uri, vers, std::move(handler));
    }

    void HTTPListener::addDBHandler(Method method, const char* uri, bool writeable, APIVersion vers,
                                    DBHandler handler) {
        _server->addHandler(method, uri, vers, [this, handler, writeable](RequestResponse& rq) {
            BorrowedDatabase db = getDatabase(rq, rq.path(0), writeable);
            if ( db ) handler(rq, db);
        });
    }

    pair<string, C4CollectionSpec> HTTPListener::parseKeySpace(slice keySpace) {
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

    BorrowedDatabase HTTPListener::getDatabase(RequestResponse& rq, const string& dbName, bool writeable) {
        BorrowedDatabase db = databaseNamed(dbName, writeable);
        if ( !db ) {
            if ( isValidDatabaseName(dbName) ) rq.respondWithStatus(HTTPStatus::NotFound, "No such database");
            else
                rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid database name");
        }
        return db;
    }

}  // namespace litecore::REST

#endif
