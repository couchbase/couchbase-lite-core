//
// LWSContext.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Address.hh"
#include "fleece/slice.hh"
#include <memory>
#include <thread>

// libwebsocket opaque structs:
struct lws;
struct lws_context;
struct lws_protocols;

namespace litecore { namespace websocket {

    /** Singleton that manages the libwebsocket context and event thread. */
    class LWSContext {
    public:
        static void initialize(const ::lws_protocols protocols[]);

        // null until initialize() is called
        static LWSContext* instance;

        bool isOpen()                          {return _context != nullptr;}

        ::lws_context* context() const         {return _context;}

        ::lws* connect(const repl::Address &_address,
                       const char *protocol,
                       fleece::slice pinnedServerCert,
                       void* opaqueUserData);

    private:
        LWSContext(const struct lws_protocols protocols[]);
        static void logCallback(int level, const char *message);
        static fleece::alloc_slice getSystemRootCertsPEM();

        ::lws_context*               _context {nullptr};
        std::unique_ptr<std::thread> _thread;
    };

} }
