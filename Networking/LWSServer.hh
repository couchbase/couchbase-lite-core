//
// LWSServer.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "c4Socket.h"
#include <condition_variable>
#include <mutex>
#include <memory>

struct lws;
struct lws_http_mount;
struct lws_vhost;

namespace litecore { namespace net {
    class LWSResponder;

    /** Abstract base class class of servers/listeners based on libwebsockets. */
    class LWSServer : public fleece::RefCounted {
    public:
        LWSServer();

        void start(uint16_t port, const char *hostname =nullptr);
        
        virtual void stop();

        C4Address address() const;

        virtual int onEvent(lws*, int callback_reason, void *user, void *in, size_t len);

    protected:
        virtual ~LWSServer();

        // Called when there's an incoming connection; should create a LWSResponder on it.
        virtual bool createResponder(lws *client) =0;

        virtual void dispatchRequest(LWSResponder*) =0;

        virtual const char *className() const noexcept      {return "LWSServer";}

    private:
        void createdVHost(lws_vhost*);
        void notifyStartStop(bool started);

        std::mutex _mutex;
        std::condition_variable _condition;
        bool _started {false};
        lws_http_mount* _mounts {nullptr};
        lws_vhost* _vhost {nullptr};

        friend class LWSContext;
        friend class LWSResponder;
        friend class LWSServerWebSocket;
    };

} }
