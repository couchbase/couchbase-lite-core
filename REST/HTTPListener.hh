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
#include "DatabaseRegistry.hh"
#include "HTTPTypes.hh"
#include "Server.hh"
#include "fleece/InstanceCounted.hh"
#include "fleece/RefCounted.hh"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

C4_ASSUME_NONNULL_BEGIN

namespace fleece {
    class JSONEncoder;
}

namespace litecore::net {
    struct Address;
    class TCPSocket;
}  // namespace litecore::net

namespace litecore::websocket {
    class Headers;
}

namespace litecore::REST {
    using fleece::RefCounted;
    using fleece::Retained;
    using namespace litecore::net;

    class Request;

    /** Listener subclass that serves HTTP requests. */
    class HTTPListener
        : public RefCounted
        , public InstanceCountedIn<HTTPListener>
        , protected Server::Delegate {
      public:
        explicit HTTPListener(const C4ListenerConfig&);
        ~HTTPListener() override;

        void setDelegate(C4Listener* d) { _delegate = d; }

        void stop();

        uint16_t port() const { return _server->port(); }

        /** My root URL, or the URL of a database. */
        virtual std::vector<Address> addresses(C4Database* C4NULLABLE dbOrNull        = nullptr,
                                               bool                   webSocketScheme = false) const;

        int connectionCount();

        int activeConnectionCount() { return (int)tasks().size(); }

        bool registerDatabase(C4Database* db, std::optional<std::string> name = std::nullopt,
                              C4ListenerDatabaseConfig const* C4NULLABLE dbConfig = nullptr);
        bool unregisterDatabase(C4Database*);
        bool registerCollection(const std::string& name, C4CollectionSpec const& collection);
        bool unregisterCollection(const std::string& name, C4CollectionSpec const& collection);

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

            /// Should return true if the task should be included in `tasks()`.
            virtual bool listed() { return !finished(); }

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

        Retained<TLSContext>              createTLSContext(const C4TLSConfig* C4NULLABLE);
        static Retained<crypto::Identity> loadTLSIdentity(const C4TLSConfig* C4NULLABLE);

        Server* server() const { return _server.get(); }

        unsigned registerTask(Task*);
        void     unregisterTask(Task*);

        // Socket::Delegate API
        void handleConnection(std::unique_ptr<ResponderSocket>) override;

        virtual HTTPStatus handleRequest(Request&, websocket::Headers&, std::unique_ptr<ResponderSocket>&) = 0;

        void writeResponse(HTTPStatus, websocket::Headers const&, TCPSocket*);

        std::string findMatchingSyncProtocol(DatabaseRegistry::DBShare const&, std::string_view clientProtocols);

        C4ListenerConfig const _config;
        C4Listener* C4NULLABLE _delegate = nullptr;
        std::string            _serverName, _serverVersion;
        DatabaseRegistry       _registry;
        std::mutex             _mutex;

      private:
        void stopTasks();

        Retained<crypto::Identity> _identity;
        Retained<Server>           _server;
        std::set<Retained<Task>>   _tasks;
        std::condition_variable    _tasksCondition;
        unsigned                   _nextTaskID{1};
    };

}  // namespace litecore::REST

C4_ASSUME_NONNULL_END

#endif
