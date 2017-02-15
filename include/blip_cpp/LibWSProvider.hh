//
//  LibWSProvider.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct ws_base_s;

namespace litecore { namespace websocket {

    /** libws-based WebSocket provider. */
    class LibWSProvider : public Provider {
    public:
        LibWSProvider();
        virtual ~LibWSProvider();

        virtual void addProtocol(const std::string &protocol) override;

        virtual Connection* connect(const Address&, Delegate&) override;

        /** Asynchronously starts the event loop on a new background thread. */
        void startEventLoop();

        /** Asynchronously stops the event loop, without waiting for it to complete. */
        void stopEventLoop();

        /** Synchronously stops the event loop and waits for it to complete. */
        virtual void close() override;

        /** Runs the event loop in the current thread. Does not return till the provider is closed. */
        void runEventLoop();

    protected:
        friend class LibWSConnection;

        struct ::ws_base_s* base() {return _base;}

    private:
        struct ::ws_base_s *_base {nullptr};
        std::vector<std::string> _protocols;
        std::unique_ptr<std::thread> _eventLoopThread;
    };

} }
