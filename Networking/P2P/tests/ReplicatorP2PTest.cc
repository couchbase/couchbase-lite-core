//
// ReplicatorP2PTest.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ReplicatorAPITest.hh"
#include "c4PeerDiscovery.hh"
#include "c4Replicator.hh"
#include "c4Socket+Internal.hh"
#include "AppleBonjourPeer.hh"    //TEMP shouldn't need this
#include "AppleBluetoothPeer.hh"  //TEMP shouldn't need this
#include "Logging.hh"
#include "TestsCommon.hh"
#include "CatchHelper.hh"
#include <semaphore>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::p2p;

class ReplicatorP2PTest
    : public ReplicatorAPITest
    , public C4PeerDiscovery::Observer {
  public:
    ReplicatorP2PTest() {
        //        InitializeBonjourProvider("couchbase-p2p");
        InitializeBluetoothProvider("couchbase-p2p");
        C4PeerDiscovery::addObserver(this);
        _mayGoOffline = true;  // Bluetooth is prone to this
    }

    ~ReplicatorP2PTest() {
        shutdown();
        C4PeerDiscovery::removeObserver(this);
    }

    // Starts browsing & publishing, and waits for them to start up.
    void start(bool publish) {
        Assert(!_browsing && !_publishing && !_browseError && !_publishError);
        unique_lock lock(_mutex);
        C4PeerDiscovery::startBrowsing();
        if ( publish ) C4PeerDiscovery::startPublishing("P2PTest", 0, {});

        _cond.wait(lock, [&] { return (_browsing && (!publish || _publishing)) || _browseError || _publishError; });
        REQUIRE(_browseError == kC4NoError);
        REQUIRE(_publishError == kC4NoError);
    }

    // Stops browsing & publishing, and waits for them to stop.
    void shutdown() {
        if ( _browsing || _publishing ) {
            unique_lock lock(_mutex);
            C4PeerDiscovery::stopBrowsing();
            C4PeerDiscovery::stopPublishing();

            _cond.wait(lock, [&] { return !_browsing && !_publishing; });
        }
    }

    // Finds a peer URL, waiting until one is discovered.
    string findAPeerURL() {
        Assert(_browsing);
        unique_lock lock(_mutex);
        _peerURL = "";
        if ( auto peers = C4PeerDiscovery::peers(); !peers.empty() ) { resolveURL(peers.begin()->second); }

        _cond.wait(lock, [&] { return !_peerURL.empty() || _peerURLError || !_browsing; });
        REQUIRE(!_peerURL.empty());
        return _peerURL;
    }

    void resolveURL(C4Peer* peer) {
        if ( !_resolvingPeer ) {
            _resolvingPeer = peer;
            peer->resolveURL([this](string_view url, C4SocketFactory const* factory, C4Error error) {
                if ( _peerURL.empty() && !_peerURLError ) {
                    unique_lock lock(_mutex);
                    _peerURL           = url;
                    _peerSocketFactory = factory;
                    _peerURLError      = error;
                    _cond.notify_all();
                }
            });
        }
    }

    void replicateWithPeer(string_view peerURL) {
        net::Address address{peerURL};
        _sg.address                  = address;
        _sg.remoteDBName             = "db"_sl;
        C4ReplicationCollection coll = {kC4DefaultCollectionSpec, kC4OneShot, kC4Disabled};
        replicate([&](C4ReplicatorParameters& params) {
            params.socketFactory                  = _peerSocketFactory;
            params.collections                    = &coll;
            params.collectionCount                = 1;
            params.collections[0].pushFilter      = _pushFilter;
            params.collections[0].pullFilter      = _pullFilter;
            params.collections[0].callbackContext = this;
        });
    }

    //---- C4PeerDiscovery::Observer callbacks:

    void browsing(C4PeerDiscoveryProvider* provider, bool active, C4Error error) override {
        unique_lock lock(_mutex);
        if ( active ) {
            Log("*** %s browsing started", provider->name.c_str());
            _browsing = true;
        } else if ( !error ) {
            Log("*** %s browsing stopped!", provider->name.c_str());
            _browsing = false;
        } else {
            Warn("Browsing failed: %s", error.description().c_str());
            _browseError = error;
            _browsing    = false;
        }
        _cond.notify_all();
    }

    void addedPeer(C4Peer* peer) override {
        Log("*** Found %s peer %s \"%s\"", peer->provider->name.c_str(), peer->id.c_str(), peer->displayName().c_str());
        unique_lock lock(_mutex);
        resolveURL(peer);
    }

    void removedPeer(C4Peer* peer) override {
        Log("*** Removed %s peer %s", peer->provider->name.c_str(), peer->id.c_str());
    }

    void publishing(C4PeerDiscoveryProvider* provider, bool active, C4Error error) override {
        unique_lock lock(_mutex);
        if ( active ) {
            Log("*** %s publishing started", provider->name.c_str());
            _publishing = true;
        } else {
            _publishing   = false;
            _publishError = error;
            if ( !error ) {
                Log("*** %s publishing stopped!", provider->name.c_str());
            } else {
                Warn("%s publishing failed: %s", provider->name.c_str(), error.description().c_str());
            }
        }
        _cond.notify_all();
    }

    bool incomingConnection(C4Peer* fromPeer, C4Socket* socket) override {
        if ( !_allowIncoming ) {
            Warn("*** Rejected incoming connection");
            return false;
        }

        Log("*** Incoming connection from %s!", fromPeer->id.c_str());
        C4ReplicatorParameters params{};
        params.onStatusChanged = [](C4Replicator*, C4ReplicatorStatus status, void* context) {
            Log("--- Incoming replication changed status, %d", status.level);
        };
        params.callbackContext                = this;
        C4ReplicationCollection coll          = {kC4DefaultCollectionSpec, kC4Passive, kC4Passive};
        params.collections                    = &coll;
        params.collectionCount                = 1;
        params.collections[0].callbackContext = this;

        _incomingRepl = c4repl_newWithSocket(db, socket, params, nullslice, ERROR_INFO());
        REQUIRE(_incomingRepl);
        _incomingRepl->start();
        return true;
    }

    mutex                  _mutex;
    condition_variable     _cond;
    bool                   _browsing     = false;
    bool                   _publishing   = false;
    C4Error                _browseError  = {};
    C4Error                _publishError = {};
    Retained<C4Peer>       _resolvingPeer;
    string                 _peerURL;
    C4SocketFactory const* _peerSocketFactory = nullptr;
    C4Error                _peerURLError      = {};
    bool                   _allowIncoming     = false;
    c4::ref<C4Replicator>  _incomingRepl;
};

#pragma mark - THE TESTS:

TEST_CASE_METHOD(ReplicatorP2PTest, "P2P Push DB") {
    importJSONLines(sFixturesDir + "names_100.json");

    start(false);
    string peerURL = findAPeerURL();
    cout << "Peer URL: " << peerURL << endl;
    replicateWithPeer(peerURL);
}

TEST_CASE_METHOD(ReplicatorP2PTest, "P2P Accept Connections") {
    _allowIncoming = true;
    start(true);
    this_thread::sleep_for(1h);
}
