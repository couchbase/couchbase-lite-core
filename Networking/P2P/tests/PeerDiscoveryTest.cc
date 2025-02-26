//
// Created by Jens Alfke on 2/4/25.
//

#include "c4PeerDiscovery.hh"
#include "PeerDiscovery+AppleDNSSD.hh"  //TEMP shouldn't need this
#include "PeerDiscovery+AppleBT.hh"     //TEMP shouldn't need this
#include "Logging.hh"
#include "TestsCommon.hh"
#include "CatchHelper.hh"
#include <semaphore>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore::p2p;

class P2PTest : public C4PeerDiscovery::Observer {
  public:
    P2PTest() {
        InitializeBonjourProvider("couchbase-p2p");
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
#if 0
        peer->monitorMetadata(true);
#else
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
#endif
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

TEST_CASE_METHOD(P2PTest, "P2P Browser", "[P2P]") {
    C4Peer::Metadata md;
    md["foo"] = alloc_slice("Foobar Baz");
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

//TEMP: Just here to expose crashes that occur after the real test completes
TEST_CASE("P2P Browser 2", "[P2P]") { this_thread::sleep_for(2s); }
