//
//  BLIPTest.cpp
//  ReplicatorTest
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "LibWSProvider.hh"
#include "MockWSProvider.hh"
#include "BLIPConnection.hh"
#include "Actor.hh"
#include "Logging.hh"
#include <algorithm>
#include <atomic>
#include <iostream>

using namespace litecore::websocket;
using namespace litecore;
using namespace fleece;


#define MOCK_WS 0

static const size_t kNumEchoers = MOCK_WS ? 5 : 100;
static const size_t kMessageSize = MOCK_WS ? 32 : 300 * 1024;

static std::atomic<int> sResponsesToReceive(kNumEchoers);
static std::atomic<int> sResponsesToSend(kNumEchoers);

class Echoer : public Actor {
public:
    Echoer(blip::Connection *conn, int number)
    :_connection(conn)
    ,_number(number)
    { }

    void send(size_t messageSize)       {enqueue(&Echoer::_send, messageSize);}

protected:

    void _send(size_t messageSize) {
        blip::MessageBuilder msg({{"Profile"_sl, "echo"_sl}});
        msg.addProperty("Sender"_sl, "BlipTest"_sl);
        uint8_t buffer[256];
        for (int i=0; i<256; i++)
            buffer[i] = (uint8_t)i;
        for (ssize_t remaining = kMessageSize; remaining > 0; remaining -= sizeof(buffer))
            msg << slice(buffer, std::min((ssize_t)sizeof(buffer), remaining));
        auto r = _connection->sendRequest(msg);
        Log("** Echoer %d sent BLIP request", _number);
        onReady(r, [this](blip::MessageIn *response) {
            std::cerr << "** BLIP response #" << response->number() << " onComplete callback\n";
            slice body = response->body();
            bool ok = true;
            for (size_t i = 0; i < body.size; i++) {
                if (body[i] != (i & 0xff)) {
                    Warn("Invalid body; byte at offset %zu is %02x; should be %02x",
                         i, body[i], (unsigned)(i & 0xff));
                    ok = false;
                }
            }
            if (ok) {
                int n = --sResponsesToReceive;
                Log("** Echoer %d got response OK! (%d remaining)", _number, n);
                if (sResponsesToSend == 0 && sResponsesToReceive == 0)
                    Log("******** DONE ********\n\n");
            }
        });
    }

private:
    Retained<blip::Connection> _connection;
    const int _number;
};


class BlipTest : public litecore::blip::ConnectionDelegate {
public:
    virtual void onConnect() override {
        std::cerr << "** BLIP Connected\n";
        for (int i = 1; i <= kNumEchoers; ++i) {
            Retained<Echoer> e = new Echoer(connection(), i);
            e->send(kMessageSize * i);
        }
    }
    virtual void onError(int errcode, fleece::slice reason) override {
        Log("** BLIP error: %s (%d)", reason.asString().c_str(), errcode);
    }
    virtual void onClose(int status, fleece::slice reason) override {
        Log("** BLIP closed: %s (status %d)", reason.asString().c_str(), status);
    }

    virtual void onRequestReceived(blip::MessageIn *msg) override {
        int n = --sResponsesToSend;
        Log("** BLIP request #%llu received: %zu bytes (%d remaining)", msg->number(), msg->body().size, n);
        if (!msg->noReply()) {
            blip::MessageBuilder out(msg);
            out << msg->body();
            msg->respond(out);
        }
        if (sResponsesToSend == 0 && sResponsesToReceive == 0)
            Log("******** DONE ********\n\n");
    }

    virtual void onResponseReceived(blip::MessageIn *msg) override {
        Log("** BLIP response #%llu received", msg->number());
    }
};


int main(int argc, const char * argv[]) {
    BlipTest test;
#if MOCK_WS
    MockWSProvider provider;
#else
    LibWSProvider provider;
#endif
    Retained<blip::Connection> connection(new blip::Connection(Address("localhost", 1234), provider, test));
    Log("Starting event loop...");

#if MOCK_WS
    Scheduler::sharedScheduler()->runSynchronous();
#else
    provider.runEventLoop();
#endif
    return 0;
}
