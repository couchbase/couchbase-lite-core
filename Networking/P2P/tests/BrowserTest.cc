//
// Created by Jens Alfke on 2/4/25.
//

#include "Browser.hh"
#include "BonjourBrowser.hh" //TEMP shouldn't need this
#include "TestsCommon.hh"
#include "CatchHelper.hh"
#include <semaphore>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore::p2p;


TEST_CASE("P2P Browser", "[P2P]") {
    Retained<Browser> browser;
    binary_semaphore sem{0};

    auto observer = [&](Browser& b, Browser::Event event, Peer* peer) {
        if (peer)
            Log("EVENT: %s, peer %p '%s'", Browser::kEventNames[event], peer, peer->name().c_str());
        else
            Log("EVENT: %s", Browser::kEventNames[event]);
        CHECK(&b == browser.get());
        if (event == Browser::BrowserStopped)
            sem.release();
    };
    browser = make_retained<BonjourBrowser>("_ssh._tcp", observer);
    browser->start();

    sem.try_acquire_for(chrono::seconds(5));    // wait five seconds for test to run, then stop
    browser->stop();
    sem.acquire();
}
