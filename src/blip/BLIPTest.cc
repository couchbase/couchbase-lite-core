//
//  BLIPTest.cpp
//  ReplicatorTest
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "LibWSProvider.hh"
#include "BLIPConnection.hh"
#include "Logging.hh"
#include <algorithm>
#include <iostream>

using namespace litecore;
using namespace fleece;


static const size_t kMessageSize = 300 * 1024;


class BlipTest : public litecore::blip::ConnectionDelegate {
public:
    virtual void onConnect() override {
        std::cerr << "** BLIP Connected\n";
        blip::MessageBuilder msg({{"Profile"_sl, "echo"_sl}});
        msg.addProperty("Sender"_sl, "BlipTest"_sl);
        uint8_t buffer[256];
        for (int i=0; i<256; i++)
            buffer[i] = (uint8_t)i;
        for (ssize_t remaining = kMessageSize; remaining > 0; remaining -= sizeof(buffer))
            msg << slice(buffer, std::min((ssize_t)sizeof(buffer), remaining));
        auto r = connection()->sendRequest(msg);
        std::cerr << "** Sent BLIP request #" /*<< r->number()*/ << "\n";
        r->onReady([](blip::MessageIn *response) {
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
            if (ok)
                std::cerr << "   Response OK!\n";
        });
    }
    virtual void onError(int errcode, fleece::slice reason) override {
        std::cerr << "** BLIP error: " << reason.asString() << "(" << errcode << ")\n";
    }
    virtual void onClose(int status, fleece::slice reason) override {
        std::cerr << "** BLIP closed: " << reason.asString() << "(status " << status << ")\n";
    }

    virtual void onRequestReceived(blip::MessageIn *msg) override {
        std::cerr << "** BLIP request #" << msg->number() << " received: " << msg->body().size << " bytes\n";
        if (!msg->noReply()) {
            blip::MessageBuilder out(msg);
            out << msg->body();
            msg->respond(out);
        }
    }

    virtual void onResponseReceived(blip::MessageIn *msg) override {
        std::cerr << "** BLIP response #" << msg->number() << " received\n";
    }
};


int main(int argc, const char * argv[]) {
    BlipTest test;
    LibWSProvider provider;
    Retained<blip::Connection> connection(new blip::Connection("localhost", 1234, provider, test));
    std::cerr << "Starting event loop...\n";
    provider.runEventLoop();
    return 0;
}
