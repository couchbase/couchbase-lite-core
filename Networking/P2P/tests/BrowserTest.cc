//
// Created by Jens Alfke on 2/4/25.
//

#include "c4PeerDiscovery.hh"
#include "PeerDiscovery+Apple.hh"  //TEMP shouldn't need this
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
        InitializeBonjourProvider("_ssh._tcp");
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
        Log("*** Added %s peer %s", peer->provider->name.c_str(), peer->id.c_str());
        peer->monitorMetadata(true);
        peer->resolveAddresses();
    }

    void removedPeer(C4Peer* peer) override {
        Log("*** Removed %s peer %s", peer->provider->name.c_str(), peer->id.c_str());
    }

    void peerMetadataChanged(C4Peer* peer) override {
        stringstream out;
        out << '{';
        for ( auto& [k, v] : peer->getAllMetadata() ) out << k << ": '" << string_view(v) << "', ";
        out << '}';
        Log("*** %s peer %s metadata changed: %s", peer->provider->name.c_str(), peer->id.c_str(), out.str().c_str());
    }

    void peerAddressesResolved(C4Peer* peer) override {
        if ( auto addrs = peer->addresses(); addrs.empty() )
            Warn("*** %s peer %s address failed to resolve: %s", peer->provider->name.c_str(), peer->id.c_str(),
                 peer->resolveError().description().c_str());
        else
            Log("*** %s peer %s address resolved to %s", peer->provider->name.c_str(), peer->id.c_str(),
                addrs[0].address.c_str());
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
    Log("--- Main thread calling startBrowsing");
    C4PeerDiscovery::startBrowsing();
    C4PeerDiscovery::startPublishing("P2PTest", 1234, {});
    sem.try_acquire_for(chrono::seconds(5));  // wait five seconds for test to run, then stop
    Log("--- Main thread calling stopBrowsing");
    C4PeerDiscovery::stopBrowsing();
    Log("--- Main thread calling stopPublishing");
    C4PeerDiscovery::stopPublishing();
    sem.acquire();
    sem.acquire();
    Log("--- Done!");
}
