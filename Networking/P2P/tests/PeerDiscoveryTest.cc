//
// Created by Jens Alfke on 2/4/25.
//

#include "c4PeerDiscovery.hh"
#include "c4Socket+Internal.hh"
#include "PeerDiscovery+AppleDNSSD.hh"  //TEMP shouldn't need this
#include "PeerDiscovery+AppleBT.hh"     //TEMP shouldn't need this
#include "Logging.hh"
#include "TestsCommon.hh"
#include "CatchHelper.hh"
#include <semaphore>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::p2p;

class P2PTest : public C4PeerDiscovery::Observer {
  public:
    P2PTest() {
        //        InitializeBonjourProvider("couchbase-p2p");
        InitializeBluetoothProvider("couchbase-p2p");
        C4PeerDiscovery::addObserver(this);
    }

    ~P2PTest() { C4PeerDiscovery::removeObserver(this); }

    void browsing(C4PeerDiscoveryProvider* provider, bool active, C4Error error) override {
        if ( active ) Log("*** %s browsing started", provider->name.c_str());
        else {
            if ( !error ) Log("*** %s browsing stopped!", provider->name.c_str());
            else
                Warn("Browsing failed: %s", error.description().c_str());
            sem.release();
        }
    }

    void addedPeer(C4Peer* peer) override {
        Log("*** Added %s peer %s \"%s\": %s", peer->provider->name.c_str(), peer->id.c_str(),
            peer->displayName().c_str(), metadataOf(peer).c_str());
    }

    void removedPeer(C4Peer* peer) override {
        Log("*** Removed %s peer %s", peer->provider->name.c_str(), peer->id.c_str());
    }

    string metadataOf(C4Peer* peer) {
        stringstream out;
        out << '{';
        for ( auto& [k, v] : peer->getAllMetadata() ) {
            out << k << ": ";
            bool printable = true;
            for ( uint8_t c : v ) {
                if ( (c < ' ' && c != '\t' && c != '\n') || c == 0x7F ) printable = false;
            }
            if ( printable ) out << '"' << string_view(v) << '"';
            else
                out << '<' << v.hexString() << ">";
            out << ", ";
        }
        out << '}';
        return out.str();
    }

    void peerMetadataChanged(C4Peer* peer) override {
        Log("*** %s peer %s metadata changed: %s", peer->provider->name.c_str(), peer->id.c_str(),
            metadataOf(peer).c_str());
    }

    void publishing(C4PeerDiscoveryProvider* provider, bool active, C4Error error) override {
        if ( active ) Log("*** %s publishing started", provider->name.c_str());
        else {
            if ( !error ) Log("*** %s publishing stopped!", provider->name.c_str());
            else
                Warn("%s publishing failed: %s", provider->name.c_str(), error.description().c_str());
            sem.release();
        }
    }

    binary_semaphore sem{0};
};

#pragma mark - RESOLVE TEST:

class P2PResolveTest : public P2PTest {
  public:
    void addedPeer(C4Peer* peer) override {
        P2PTest::addedPeer(peer);
        Retained retainedPeer(peer);
        peer->resolveURL([this, retainedPeer](string url, C4Error error) {
            if ( error ) {
                Warn("*** Failed to resolve URL of %s peer %s -- %s", retainedPeer->provider->name.c_str(),
                     retainedPeer->id.c_str(), error.description().c_str());
            } else {
                Log("*** Resolved URL of %s peer %s as <%s>", retainedPeer->provider->name.c_str(),
                    retainedPeer->id.c_str(), url.c_str());
            }
        });
    }
};

TEST_CASE_METHOD(P2PResolveTest, "P2P Resolve", "[P2P]") {
    C4Peer::Metadata md;
    md["foo"]  = alloc_slice("Foobar Baz");
    md["time"] = alloc_slice("right now");

    Log("--- Main thread calling startBrowsing");
    C4PeerDiscovery::startBrowsing();
    C4PeerDiscovery::startPublishing("P2PTest", 1234, md);
    sem.try_acquire_for(chrono::seconds(90));  // wait five seconds for test to run, then stop
    Log("--- Main thread calling stopBrowsing");
    C4PeerDiscovery::stopBrowsing();
    Log("--- Main thread calling stopPublishing");
    C4PeerDiscovery::stopPublishing();
    sem.acquire();
    sem.acquire();
    Log("--- Done!");
}

#pragma mark - CONNECT TEST

struct WebSocketLogger
    : public RefCounted
    , public websocket::Delegate {
    Retained<websocket::WebSocket> _webSocket;
    string                         _name;

    WebSocketLogger(string_view url, C4SocketFactory const* factory, const char* name) : _name(name) {
        _webSocket = repl::CreateWebSocket(alloc_slice(url), nullslice, nullptr, factory);
        _webSocket->connect(new WeakHolder<websocket::Delegate>(this));
        Log("$$$ CREATE %s", _name.c_str());
    }

    WebSocketLogger(C4Socket* socket, const char* name) : _webSocket(repl::WebSocketFrom(socket)), _name(name) {
        _webSocket->connect(new WeakHolder<websocket::Delegate>(this));
        Log("$$$ CREATE %s", _name.c_str());
    }

    void onWebSocketGotTLSCertificate(slice certData) override {}

    void onWebSocketConnect() override {
        Log("$$$ CONNECT %s", _name.c_str());
        _webSocket->send("HELLO THERE");
    }

    void onWebSocketClose(litecore::websocket::CloseStatus) override { Log("$$$ CLOSE %s", _name.c_str()); }

    /** A message has arrived. */
    void onWebSocketMessage(litecore::websocket::Message* msg) override {
        Log("$$$ MESSAGE %s : %.*s", _name.c_str(), FMTSLICE(msg->data));
    }

    /** The socket has room to send more messages. */
    void onWebSocketWriteable() override { Log("$$$ WRITEABLE %s", _name.c_str()); }
};

class P2PConnectTest : public P2PTest {
  public:
    bool _shouldConnect = true;

    void addedPeer(C4Peer* peer) override {
        P2PTest::addedPeer(peer);
        if ( _shouldConnect && !_out && peer->provider->name == "Bluetooth" ) {
            Retained retainedPeer(peer);
            peer->resolveURL([this, retainedPeer](string_view url, C4Error error) {
                if ( !url.empty() ) {
                    Log("*** Opening connection to %s peer %s", retainedPeer->provider->name.c_str(),
                        retainedPeer->id.c_str());
                    _out = make_retained<WebSocketLogger>(url, retainedPeer->provider->getSocketFactory(), "out");
                } else {
                    Warn("*** Failed to resolve URL of %s peer %s -- %s", retainedPeer->provider->name.c_str(),
                         retainedPeer->id.c_str(), error.description().c_str());
                    FAIL_CHECK("Failed to connect to peer");
                }
            });
        }
    }

    bool incomingConnection(C4Peer* peer, C4Socket* socket) override {
        Log("*** Incoming connection from %s peer %s", peer->provider->name.c_str(), peer->id.c_str());
        if ( _in ) return false;
        _in = make_retained<WebSocketLogger>(socket, "in");
        return true;
    }

    Retained<WebSocketLogger> _out, _in;
};

TEST_CASE_METHOD(P2PConnectTest, "P2P Connect", "[P2P]") {
    C4Peer::Metadata md;
    md["foo"]  = alloc_slice("Foobar Baz");
    md["time"] = alloc_slice("right now");

    Log("--- Main thread calling startBrowsing");
    C4PeerDiscovery::startBrowsing();
    C4PeerDiscovery::startPublishing("P2PTest", 1234, md);
    sem.try_acquire_for(chrono::seconds(90));  // wait five seconds for test to run, then stop
    Log("--- Main thread calling stopBrowsing");
    C4PeerDiscovery::stopBrowsing();
    Log("--- Main thread calling stopPublishing");
    C4PeerDiscovery::stopPublishing();
    sem.acquire();
    sem.acquire();
    Log("--- Done!");
}
