//
// Poller.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonportable-system-include-path"
#include "sockpp/socket.h"
#pragma clang diagnostic pop

namespace litecore::net {
    // This needs to stay here because of the platform variations of
    // socket_t and INVALID_SOCKET (Windows has them globally and
    // Unix has them in this namespace)
    using namespace sockpp;

    /** Enables async I/O by running `poll` on a background thread. */
    class Poller {
      public:
        /// The single shared instance (all that's necessary in normal use)
        static Poller& instance();

        enum Event {
            kReadable,     // Data (or EOF) has arrived
            kWriteable,    // Socket has room to write data
            kDisconnected  // Socket was closed locally or remotely, or disconnected with error
        };

        using Listener = std::function<void()>;

        /// The next time the Event is possible on the file descriptor, call the Listener.
        /// The Listener is called on a shared background thread and should return ASAP.
        /// It will not be called again -- if you need another notification, call `addListener`
        /// again (it's fine to call it from inside the callback.)
        void addListener(int fd, Event, Listener);

        /// Immediately calls (and removes) any listeners on the file descriptor.
        void interrupt(int fd);

        /// Removes all Listeners for this file descriptor.
        void removeListeners(int fd);

        // Manual controls over instances, starting and stopping -- for testing
        Poller();
        ~Poller();
        Poller& start();
        void    stop();

      private:
        explicit Poller(bool startNow) : Poller() {
            if ( startNow ) start();
        }

        bool poll();
        void callAndRemoveListener(int fd, Event);
        void _interrupt(int fd) const;

        std::mutex                                            _mutex;
        std::unordered_map<socket_t, std::array<Listener, 3>> _listeners;  // array indexed by Event
        std::thread                                           _thread;
        std::atomic_bool                                      _waiting{false};

        socket_t _interruptReadFD{INVALID_SOCKET};   // Pipe used to interrupt poll()
        socket_t _interruptWriteFD{INVALID_SOCKET};  // Other end of the pipe
    };

}  // namespace litecore::net
