//
// P2PSyncManager.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef COUCHBASE_ENTERPRISE

#    include "P2PSyncManager.hh"
#    include "c4Replicator+Pool.hh"
#    include "c4Socket+Internal.hh"
#    include "DatabasePool.hh"
#    include "SecureRandomize.hh"
#include "SmallVector.hh"
#    include "SyncListener.hh"

namespace litecore::p2p {
    using namespace std;
    using namespace litecore::REST;

    extern LogDomain P2PLog;


    CopiedReplicatorParameters::CopiedReplicatorParameters(C4ReplicatorParameters const& params)
        : C4ReplicatorParameters(params) {
        auto makeAllocated = [this](auto& s) { s = _slices.emplace_back(s); };
        makeAllocated(optionsDictFleece);
        _collections = make_unique<C4ReplicationCollection[]>(collectionCount);
        std::copy(collections, collections + collectionCount, _collections.get());
        collections = _collections.get();
        for ( size_t i = 0; i < collectionCount; i++ ) {
            makeAllocated(collections[i].collection.name);
            makeAllocated(collections[i].collection.scope);
            makeAllocated(collections[i].optionsDictFleece);
        }
    }


    SyncManager::SyncManager(DatabaseOrPool db,
                             std::span<const std::string_view> providers,
                             string_view serviceID,
                             C4ReplicatorParameters const& params)
    : Actor(P2PLog, "SyncManager")
      , _peerDiscovery(serviceID, providers)
      , _replicatorParams(params) {
        // Create or retain the database pool:
        if ( std::holds_alternative<Retained<C4Database>>(db) )
            _databasePool = make_retained<DatabasePool>(std::get<Retained<C4Database>>(db));
        else
            _databasePool = std::get<Retained<DatabasePool>>(db);
        _databaseName = DatabaseRegistry::databaseNameFromPath(_databasePool->databasePath());
        // My ID as a peer is the database's public UUID:
        _myUUID = BorrowedDatabase(_databasePool)->getPublicUUID();
        string myIDStr = _myUUID.to_string();

        // Start the sync listener:
        C4ListenerConfig listenerConfig{
                .allowPush = true,
                .allowPull = true,
        };
        _syncListener = make_retained<SyncListener>(listenerConfig);
        _syncListener->registerDatabase(_databasePool, _databaseName);
        for ( size_t i = 0; i < _replicatorParams.collectionCount; i++ )
            _syncListener->registerCollection(_databaseName,
                                              _replicatorParams.collections[i].collection);
        uint16_t port = _syncListener->port();
        Assert(port != 0);

        // Start P2P stuff!
        _peerDiscovery.addObserver(this);
        _peerDiscovery.startBrowsing();
        _peerDiscovery.startPublishing(myIDStr, port, {
            {C4Peer::kDeviceUUIDKey, alloc_slice(myIDStr)}
        });
    }


    SyncManager::~SyncManager() = default;


    void SyncManager::_stop() {
        _peerDiscovery.stopBrowsing();
        _peerDiscovery.stopPublishing();
        if ( _syncListener ) {
            _syncListener->stop();
            _syncListener = nullptr;
        }
    }


    void SyncManager::addedPeer(C4Peer* peer) {
        enqueue(FUNCTION_TO_QUEUE(SyncManager::_addedPeer), Retained{peer});
    }

    void SyncManager::removedPeer(C4Peer* peer) {
        enqueue(FUNCTION_TO_QUEUE(SyncManager::_removedPeer), Retained{peer});
    }

    void SyncManager::peerMetadataChanged(C4Peer* peer) {
        enqueue(FUNCTION_TO_QUEUE(SyncManager::_peerMetadataChanged), Retained{peer});
    }
    bool SyncManager::incomingConnection(C4Peer* peer, C4Socket* socket) {
        enqueue(FUNCTION_TO_QUEUE(SyncManager::_incomingConnection), Retained{peer}, socket);
        return true;
    }

    void SyncManager::_addedPeer(Retained<C4Peer> c4peer) {
        if (!c4peer->getAllMetadata().empty())
            _peerMetadataChanged(c4peer);
        c4peer->monitorMetadata(true);
    }

    void SyncManager::_peerMetadataChanged(Retained<C4Peer> c4peer) {
        if (alloc_slice id = c4peer->getMetadata(C4Peer::kDeviceUUIDKey); id.size == sizeof(C4UUID)) {
            auto& uuid = *static_cast<C4UUID const*>(id.buf);
            if (auto metaPeer = _metaPeers.addC4Peer(c4peer, uuid)) {
                if (metaPeer->count() == 1)
                    logInfo("MetaPeer %s online", uuid.to_string().c_str());

                if (metaPeer->taskCount() == 0 && clockwise(_myUUID, uuid))
                    connectToPeer(metaPeer);
            }
        }
    }

    void SyncManager::_removedPeer(Retained<C4Peer> c4peer) {
        if (auto metaPeer = _metaPeers.removeC4Peer(c4peer)) {
            logInfo("MetaPeer %s went offline", metaPeer->uuid.to_string().c_str());
        }
    }

    /// Handle an incoming C4Socket connection.
    /// NOTE: Regular TCP+WebSocket connections go through the SyncListener instead.
    /// This method handles other protocols, like Bluetooth.
    void SyncManager::_incomingConnection(Retained<C4Peer> fromC4Peer, C4Socket* socket) {
        Retained webSocket = repl::WebSocketFrom(socket);
        fromC4Peer->resolveURL(asynchronize("_incomingConnection",
                               [=,this](string const& url, C4SocketFactory const*, C4Error error) {
            auto peer = _metaPeers.metaPeerWithC4Peer(fromC4Peer);
            if (peer) {
                logInfo("Incoming connection from %s!", peer->uuid.to_string().c_str());
            } else {
                logInfo("Incoming connection from unknown C4Peer '%s'!", fromC4Peer->id.c_str());
            }
            if (!error) {
                auto task = _syncListener->handleWebSocket(_databaseName, webSocket, url);
                if (task && peer)
                    peer->addTask(task);
            }
        }));
    }


#pragma mark - REPLICATE TASK:

    /** Manages an active (client-side) replication with a peer. */
    class SyncManager::ReplicateTask : public HTTPListener::Task {
    public:
        explicit ReplicateTask(SyncManager* manager, MetaPeer* peer, C4Peer* c4peer)
        :Task(manager->_syncListener)
        ,_manager(manager)
        ,_peer(peer)
        ,_c4peer(c4peer)
        {
            registerTask();
            LogToAt(P2PLog, Info, "ReplicateTask #%u created, connecting to peer %s",
                    taskID(), _peer->uuid.to_string().c_str());

            Retained retainedThis(this);
            _c4peer->resolveURL([retainedThis](string const& url, C4SocketFactory const* factory,
                                                C4Error error) {
                if (error) {
                    retainedThis->stop();//TODO: Handle error
                } else {
                    retainedThis->startReplicator(url, factory);
                }
            });
        }

        bool finished() const override {
            unique_lock lock(_mutex);
            return _finished;
        }

        void stop() override {
            LogToAt(P2PLog, Info, "ReplicateTask #%u stopping...", taskID());
            unique_lock lock(_mutex);
            if ( _repl ) {
                _repl->stop();
            } else {
                _finished = true;
                unregisterTask();
            }
        }

    private:
        void startReplicator(string const& url, C4SocketFactory const* factory) {
            LogToAt(P2PLog, Info, "ReplicateTask #%u connecting to %s", taskID(), url.c_str());
            bumpTimeUpdated();
            unique_lock lock(_mutex);
            if (_finished)
                return;

            Encoder options;
            options.beginDict();
            options[kC4ReplicatorOptionRemoteDBUniqueID] = _peer->uuid.asSlice();
            options.endDict();

            CopiedReplicatorParameters params = _manager->_replicatorParams;
            params.optionsDictFleece = options.finish();
            params.socketFactory = factory;
            params.callbackContext = this;
            for (size_t i = 0; i < params.collectionCount; i++)
                params.collections[i].callbackContext = this;
            params.onStatusChanged = [](C4Replicator*,
                                        C4ReplicatorStatus status,
                                        void* context) {
                ((ReplicateTask*)context)->onReplStateChanged(status);
            };
            params.onDocumentsEnded = [](C4Replicator*,
                                         bool pushing,
                                         size_t numDocs,
                                         const C4DocumentEnded* docs[],
                                         void* context)
            {
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wunsafe-buffer-usage-in-container"
                ((ReplicateTask*)context)->onDocumentsEnded(pushing, span(docs, numDocs));
                #pragma GCC diagnostic pop
            };

            try {
                Address address{url};
                _repl = NewReplicator(_manager->_databasePool, address, _manager->_databaseName, params);
                //_replicator->setProgressLevel(kC4ReplProgressPerDocument);
                _repl->start();
                if ( _repl )   // unless onReplStateChanged was already called and I cleared the ref
                    onReplStateChanged(_repl->getStatus());
            } catch ( ... ) {
                LogToAt(P2PLog, Info, "ReplicateTask #%u failed to start!", taskID());
                unregisterTask();
            }
        }

        void onReplStateChanged(const C4ReplicatorStatus& status) {
            static constexpr const char* kStatusName[] = { // indexed by C4ReplicatorActivityLevel
                "stopped", "offline", "connecting", "idle", "active", "stopping"};

            bumpTimeUpdated();
            alloc_slice message = c4error_getMessage(status.error);

            unique_lock lock(_mutex);
            const char* statusName = kStatusName[status.level];
            if (status.error) {
                LogToAt(P2PLog, Error, "ReplicateTask #%u is %s: %.*s [%d,%d]",
                    taskID(), statusName, FMTSLICE(_message),
                    status.error.domain, status.error.code);
            } else if (status.level != _status.level) {
                if (status.level == kC4Stopped)
                    statusName = "finished";
                LogToAt(P2PLog, Info, "ReplicateTask #%u is %s",
                    taskID(), statusName);
            }

            _status  = status;
            _message = message;

            if ( status.level == kC4Stopped ) {
                _finished = true;
                _repl     = nullptr;
                unregisterTask();
            }
        }

        void onDocumentsEnded(bool pushing, span<const C4DocumentEnded*> docs) {
            // TODO
        }

        SyncManager* const      _manager;
        Retained<MetaPeer>      _peer;
        Retained<C4Peer>        _c4peer;
        Retained<C4Replicator>  _repl;
        C4ReplicatorStatus     _status {};
        alloc_slice            _message;
        bool                    _finished = false;
    };

    bool SyncManager::connectToPeer(MetaPeer* peer) {
        if (auto c4peer = peer->bestC4Peer()) {
            auto task = make_retained<ReplicateTask>(this, peer, c4peer);
            peer->addTask(task);
            return true;
        } else {
            return false;
        }
    }

}  // namespace litecore::p2p

#endif  // COUCHBASE_ENTERPRISE
