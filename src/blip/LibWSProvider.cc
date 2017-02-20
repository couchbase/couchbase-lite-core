//
//  LibWSProvider.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "LibWSProvider.hh"
#include "Logging.hh"

#include "libws.h"
#include "libws_log.h"
#include <event2/thread.h>

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <mutex>
#include <thread>

using namespace std;
using namespace fleece;

namespace litecore { namespace websocket {


#pragma mark - CONNECTION:


    /** libws-based WebSocket connection. */
    class LibWSWebSocket : public WebSocket {
    public:

        LibWSWebSocket(LibWSProvider &provider, ws_t websocket, const Address &address)
        :WebSocket(provider, address)
        ,_ws(websocket)
        { }


        ~LibWSWebSocket() {
            ws_destroy(&_ws);
        }

        
        virtual void connect() override {
            auto del = &delegate();
            ws_set_onwrite_cb(_ws, onwrite, del);
            ws_set_onmsg_cb(_ws, onmsg, del);
            ws_set_onconnect_cb(_ws, onconnect, del);
            ws_set_onclose_cb(_ws, onclose, del);
            ws_set_no_copy_cb(_ws, oncleanup, nullptr);

            auto &addr = address();
            if (ws_connect(_ws, addr.hostname.c_str(), addr.port, addr.path.c_str())) {
                ws_destroy(&_ws);
                _ws = nullptr;
                throw "connection failed";
            }
            delegate().onWebSocketStart();
        }


        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) override {
            // TODO: improve ws_close_threadsafe to take status+message
            ws_close_threadsafe(_ws);
        }


        virtual void send(slice msg, bool binary) override {
            slice copied = msg.copy();
            if (ws_threadsafe_send_msg_ex(_ws, (char*)copied.buf, copied.size, binary) != 0)
                Warn("ws_threadsafe_send_msg_ex failed!");
        }

        static void oncleanup(ws_t ws, const void *data, uint64_t datalen, void *extra) {
            slice(data, datalen).free();
        }


        static void onconnect(ws_t ws, void *context) noexcept {
            try {
                ((Delegate*)context)->onWebSocketConnect();
            } catch (...) {
                fprintf(stderr, "WARNING: WebSocketDelegate::onConnect threw an exception\n");
            }
        }

        static void onwrite(ws_t ws, void *context) noexcept {
            try {
                ((Delegate*)context)->onWebSocketWriteable();
            } catch (...) {
                fprintf(stderr, "WARNING: WebSocketDelegate::onWriteable threw an exception\n");
            }
        }

        static void onmsg(ws_t ws, char *msg, uint64_t len, int binary, void *context) noexcept {
            try {
                ((Delegate*)context)->onWebSocketMessage({msg, len}, binary);
            } catch (...) {
                fprintf(stderr, "WARNING: WebSocketDelegate::onMessage threw an exception\n");
            }
        }

        static void onclose(ws_t ws, int code, int type,
                            const char *reason, size_t reason_len,
                            void *context) noexcept
        {
            // TODO: Use type, which is WS_ERRTYPE_LIB, WS_ERRTYPE_PROTOCOL, or WS_ERRTYPE_DNS
            try {
                if (type == WS_ERRTYPE_PROTOCOL)
                    ((Delegate*)context)->onWebSocketClose(code, {reason, reason_len});
                else
                    ((Delegate*)context)->onWebSocketError(code, {reason, reason_len});
            } catch (...) {
                fprintf(stderr, "WARNING: websocket::Delegate::onClose threw an exception\n");
            }
        }

    private:
        ws_t _ws {nullptr};
    };


#pragma mark - PROVIDER:


    LibWSProvider::LibWSProvider() {
        static once_flag once;
        call_once(once, [] {
            // One-time initialization:
            evthread_use_pthreads();
            int level = LIBWS_CRIT | LIBWS_ERR | LIBWS_WARN;
            ws_set_log_cb(ws_default_log_cb);
            if (getenv("WSLog"))
                level |= LIBWS_INFO | LIBWS_DEBUG | LIBWS_TRACE;
            ws_set_log_level(level);
        });

        if (ws_global_init(&_base) != 0)
            throw "Failed to init ws_base";
    }


    LibWSProvider::~LibWSProvider() {
        if (_base)
            ws_global_destroy(&_base);
    }


    void LibWSProvider::addProtocol(const std::string &protocol) {
        if (find(_protocols.begin(), _protocols.end(), protocol) == _protocols.end())
            _protocols.push_back(protocol);
    }


    WebSocket* LibWSProvider::createWebSocket(const Address &address)
    {
        ws_t ws;
        if (ws_init(&ws, _base) != 0)
            throw "Failed to init websocket state";
        for (auto &proto : _protocols)
            ws_add_subprotocol(ws, proto.c_str());
        return new LibWSWebSocket(*this, ws, address);
    }

    void LibWSProvider::runEventLoop() {
        ws_base_service_blocking(_base);
    }

    void LibWSProvider::startEventLoop() {
        if (!_eventLoopThread)
            _eventLoopThread.reset( new std::thread([this]{runEventLoop();}) );
    }

    void LibWSProvider::stopEventLoop() {
        ws_base_quit(_base, true);
    }

    void LibWSProvider::close() {
        stopEventLoop();
        if (_eventLoopThread)
            _eventLoopThread->join();
    }

} }
