//
// Poller.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "sockpp/platform.h"
#include "sockpp/socket.h"

namespace litecore { namespace net {
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
            kReadable, kWriteable
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
        void stop();

    private:
        Poller(bool startNow)               :Poller() {if (startNow) start();}
        bool poll();
        void callAndRemoveListener(int fd, Event);
        
        std::mutex _mutex;
        std::unordered_map<socket_t, std::array<Listener,2>> _listeners;
        std::thread _thread;
        std::atomic_bool _waiting {false};

        socket_t _interruptReadFD  {INVALID_SOCKET}; // Pipe used to interrupt poll()
        socket_t _interruptWriteFD {INVALID_SOCKET}; // Other end of the pipe
    };

} }
