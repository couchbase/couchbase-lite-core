//
// BLIPTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "LibWSProvider.hh"
#include "MockProvider.hh"
#include "LoopbackProvider.hh"
#include "BLIPConnection.hh"
#include "Actor.hh"
#include "Logging.hh"
#include <algorithm>
#include <atomic>
#include <iostream>

using namespace litecore::websocket;
using namespace litecore;
using namespace fleece;

#define LIBWS_PROVIDER 0
#define LOOPBACK_PROVIDER 1
#define MOCK_PROVIDER 2

#define PROVIDER LOOPBACK_PROVIDER

#define LATENCY 0.010     // simulated latency for loopback provider


static const size_t kNumEchoers = (PROVIDER==LIBWS_PROVIDER)  ? 100 : 100;
static const size_t kMessageSize = 300 * 1024;

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
            Log("** BLIP response #%llu onComplete callback", response->number());
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
    BlipTest(size_t numEchoers)
    :_numEchoers(numEchoers)
    { }

    virtual void onConnect() override {
        Log("** BLIP Connected");
        for (int i = 1; i <= _numEchoers; ++i) {
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

    const size_t _numEchoers;
};


int main(int argc, const char * argv[]) {
    BlipTest test(kNumEchoers);
#if PROVIDER == LOOPBACK_PROVIDER
    LoopbackProvider provider(LATENCY);
#elif PROVIDER == MOCK_PROVIDER
    MockProvider provider;
#else
    LibWSProvider provider;
#endif

    auto webSocket = provider.createWebSocket(Address("localhost", 1234));
    Retained<blip::Connection> connection(new blip::Connection(webSocket, test));

#if PROVIDER == LOOPBACK_PROVIDER
    BlipTest test2(0);
    auto webSocket2 = provider.createWebSocket(Address("remote", 4321));
    Retained<blip::Connection> connection2(new blip::Connection(webSocket2, test2));
    provider.connect(webSocket, webSocket2);
#endif

    Log("Starting event loop...");
#if PROVIDER == LIBWS_PROVIDER
    provider.runEventLoop();
#else
    Scheduler::sharedScheduler()->runSynchronous();
#endif
    return 0;
}
