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
#include "Address.hh"
#include "Headers.hh"
#include "netUtils.hh"
#include "Replicator.hh"
#include "Request.hh"
#include "TCPSocket.hh"
#include "TLSContext.hh"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "slice_stream.hh"

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace litecore::crypto;

namespace litecore::REST {
    using namespace net;

    HTTPListener::HTTPListener(const C4ListenerConfig& config)
        : _config(config)
        , _serverName(config.serverName ? slice(config.serverName) : "CouchbaseLite"_sl)
        , _serverVersion(config.serverVersion ? slice(config.serverVersion) : alloc_slice(c4_getVersion()))
        , _server(new Server(*this)) {
        _server->start(config.port, config.networkInterface, createTLSContext(config.tlsConfig).get());
    }

    HTTPListener::~HTTPListener() { stop(); }

    void HTTPListener::stop() {
        if ( _server ) {
            _server->stop();
            stopTasks();
            _registry.closeDatabases();
            _server = nullptr;
        }
    }

    vector<Address> HTTPListener::addresses(C4Database* dbOrNull, bool webSocketScheme) const {
        optional<string> dbNameStr;
        slice            dbName;
        if ( dbOrNull ) {
            dbNameStr = _registry.nameOfDatabase(dbOrNull);
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

#    pragma mark - CONNECTIONS:

    int HTTPListener::connectionCount() { return _server->connectionCount(); }

    bool HTTPListener::registerDatabase(C4Database* db, optional<string> name,
                                        C4ListenerDatabaseConfig const* dbConfigP) {
        C4ListenerDatabaseConfig dbConfig;
        if ( !dbConfigP ) {
            dbConfig  = {.allowPush       = _config.allowPush,
                         .allowPull       = _config.allowPull,
                         .enableDeltaSync = _config.enableDeltaSync};
            dbConfigP = &dbConfig;
        }
        return _registry.registerDatabase(db, name, *dbConfigP);
    }

    bool HTTPListener::registerDatabase(DatabasePool* db, optional<string> name,
                                        C4ListenerDatabaseConfig const* dbConfigP) {
        C4ListenerDatabaseConfig dbConfig;
        if ( !dbConfigP ) {
            dbConfig  = {.allowPush       = _config.allowPush,
                         .allowPull       = _config.allowPull,
                         .enableDeltaSync = _config.enableDeltaSync};
            dbConfigP = &dbConfig;
        }
        return _registry.registerDatabase(db, name, *dbConfigP);
    }

    bool HTTPListener::unregisterDatabase(C4Database* db) { return _registry.unregisterDatabase(db); }

    bool HTTPListener::registerCollection(const std::string& name, C4CollectionSpec const& collection) {
        return _registry.registerCollection(name, collection);
    }

    bool HTTPListener::unregisterCollection(const std::string& name, C4CollectionSpec const& collection) {
        return _registry.unregisterCollection(name, collection);
    }

    void HTTPListener::handleConnection(std::unique_ptr<ResponderSocket> socket) {
        // Parse HTTP request:
        Request rq(socket.get());
        if ( C4Error err = rq.socketError() ) {
            string peer = socket->peerAddress();
            if ( err == C4Error{NetworkDomain, kC4NetErrConnectionReset} ) {
                c4log(ListenerLog, kC4LogInfo, "End of socket connection from %s (closed by peer)", peer.c_str());
            } else {
                c4log(ListenerLog, kC4LogError, "Error reading HTTP request from %s: %s", peer.c_str(),
                      err.description().c_str());
            }
            return;
        }

        websocket::Headers headers;
        headers.add("Date", timestamp());
        headers.add("Server", _serverName + "/" + _serverVersion);
        HTTPStatus status;

        // HTTP auth:
        if ( auto authCallback = _config.httpAuthCallback ) {
            if ( !authCallback(_delegate, rq.header("Authorization"), _config.callbackContext) ) {
                c4log(ListenerLog, kC4LogInfo, "Authentication failed");
                headers.add("WWW-Authenticate", "Basic charset=\"UTF-8\"");
                writeResponse(HTTPStatus::Unauthorized, headers, socket.get());
                return;
            }
        }

        // Handle the request:
        try {
            status = handleRequest(rq, headers, socket);
        } catch ( ... ) {
            C4Error error = C4Error::fromCurrentException();
            c4log(ListenerLog, kC4LogWarning, "HTTP handler caught C++ exception: %s", error.description().c_str());
            status = StatusFromError(error);
        }
        if ( socket ) writeResponse(status, headers, socket.get());
    }

    void HTTPListener::writeResponse(HTTPStatus status, websocket::Headers const& headers, TCPSocket* socket) {
        const char* statusMessage = StatusMessage(status);
        if ( !statusMessage ) statusMessage = "";
        stringstream response;
        response << "HTTP/1.1 " << int(status) << ' ' << statusMessage << "\r\n";
        headers.forEach([&](slice name, slice value) { response << name << ": " << value << "\r\n"; });
        response << "\r\n";
        (void)socket->write(response.str());
    }

    string HTTPListener::findMatchingSyncProtocol(DatabaseRegistry::DBShare const& share, string_view clientProtocols) {
        auto boolToMode      = [](bool enabled) { return enabled ? kC4Passive : kC4Disabled; };
        auto serverProtocols = repl::Replicator::compatibleProtocols(share.pool->getConfiguration().flags,
                                                                     boolToMode(share.config.allowPush),
                                                                     boolToMode(share.config.allowPull));

        for ( auto protocol : split(clientProtocols, ",") ) {
            if ( std::ranges::find(serverProtocols, protocol) != serverProtocols.end() ) return string(protocol);
        }
        return "";
    }

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

    void HTTPListener::Task::writeDescription(JSONEncoder& json) {
        unsigned long age = ::time(nullptr) - _timeStarted;
        json.writeFormatted("task_id: %u, age_secs: %lu", _taskID, age);
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
        vector<Retained<Task>> result;
        for ( auto i = _tasks.begin(); i != _tasks.end(); ) {
            if ( (*i)->listed() ) result.push_back(*i++);
            else
                i = _tasks.erase(i);  // Clean up old finished tasks
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

}  // namespace litecore::REST

#endif
