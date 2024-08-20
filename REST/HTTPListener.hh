//
// HTTPListener.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4DatabaseTypes.h"
#include "Listener.hh"
#include "Server.hh"
#include "FilePath.hh"
#include "fleece/InstanceCounted.hh"
#include "fleece/RefCounted.hh"
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

C4_ASSUME_NONNULL_BEGIN

namespace litecore::REST {
    using fleece::RefCounted;
    using fleece::Retained;
    class Server;

    /** Listener subclass that serves HTTP requests.
        The HTTP work is done by a Server object. */
    class HTTPListener : public Listener {
      public:
        explicit HTTPListener(const Config&);
        ~HTTPListener() override;

        void setDelegate(C4Listener* d) { _delegate = d; }

        virtual void stop();

        uint16_t port() const { return _server->port(); }

        /** My root URL, or the URL of a database. */
        virtual std::vector<net::Address> addresses(C4Database* C4NULLABLE dbOrNull        = nullptr,
                                                    bool                   webSocketScheme = false) const;

        int connectionCount() override;

        int activeConnectionCount() override { return (int)tasks().size(); }

        /** An asynchronous task (like a replication). */
        class Task
            : public RefCounted
            , public InstanceCountedIn<Task> {
          public:
            explicit Task(HTTPListener* listener) : _listener(listener) {}

            HTTPListener* listener() const { return _listener; }

            /// A unique integer ID, assigned when registerTask is called (until then, 0.)
            unsigned taskID() const { return _taskID; }

            /// The time activity last occurred (i.e. when bumpTimeUpdated was called.)
            time_t timeUpdated() const { return _timeUpdated; }

            /// Call this when activity occurs: it sets timeUpdated to now.
            void bumpTimeUpdated();

            /// Should return true if the Task has completed its work.
            virtual bool finished() const = 0;

            /// Should add keys+values to the encoder to describe the Task.
            virtual void writeDescription(fleece::JSONEncoder&);

            /// Should stop whatever activity the Task is doing.
            virtual void stop() = 0;

            void registerTask();    ///< Call this before returning from handler
            void unregisterTask();  ///< Call this when the Task is finished.

          protected:
            mutable std::recursive_mutex _mutex;

          private:
            HTTPListener* const _listener;
            unsigned            _taskID{0};
            std::atomic<time_t> _timeStarted{0};
            time_t              _timeUpdated{0};
        };

        /// The currently-running tasks.
        std::vector<Retained<Task>> tasks();

      protected:
        friend class Task;

        Retained<net::TLSContext>         createTLSContext(const C4TLSConfig*);
        static Retained<crypto::Identity> loadTLSIdentity(const C4TLSConfig*);

        Server* server() const { return _server.get(); }

        BorrowedDatabase getDatabase(RequestResponse& rq, const std::string& dbName, bool writeable);

        unsigned registerTask(Task*);
        void     unregisterTask(Task*);

        using APIVersion = Server::APIVersion;
        using Handler    = Server::Handler;
        using DBHandler  = std::function<void(RequestResponse&, C4Database*)>;

        void addHandler(net::Method, const char* uri, APIVersion, Handler);
        void addDBHandler(net::Method, const char* uri, bool writeable, APIVersion, DBHandler);

        std::string _serverName, _serverVersion;

      private:
        static std::pair<std::string, C4CollectionSpec> parseKeySpace(slice keySpace);

        Retained<crypto::Identity> _identity;
        Retained<Server>           _server;
        C4Listener* C4NULLABLE     _delegate = nullptr;
        std::set<Retained<Task>>   _tasks;
        unsigned                   _nextTaskID{1};
    };

}  // namespace litecore::REST

C4_ASSUME_NONNULL_END

#endif
