//
// RESTListener.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#pragma once
#include "c4DatabaseTypes.h"
#include "c4Listener.hh"
#include "Listener.hh"
#include "Server.hh"
#include "FilePath.hh"
#include "RefCounted.hh"
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace litecore { namespace REST {
    using fleece::RefCounted;
    using fleece::Retained;
    class Server;


    /** Listener subclass that serves (some of) the venerable CouchDB REST API.
        The HTTP work is done by a Server object. */
    class RESTListener : public Listener {
    public:
        explicit RESTListener(const Config&);
        ~RESTListener();

        virtual void stop();

        uint16_t port() const                       {return _server->port();}

        /** My root URL, or the URL of a database. */
        virtual std::vector<net::Address> addresses(C4Database *dbOrNull =nullptr,
                                                    C4ListenerAPIs api = kC4RESTAPI) const;

        virtual int connectionCount() override;
        virtual int activeConnectionCount() override    {return (int)tasks().size();}

        /** Given a database name (from a URI path) returns the filesystem path to the database. */
        bool pathFromDatabaseName(const std::string &name, FilePath &outPath);

        /** An asynchronous task (like a replication). */
        class Task : public RefCounted {
        public:
            explicit Task(RESTListener* listener)    :_listener(listener) { }

            RESTListener* listener() const  {return _listener;}
            unsigned taskID() const     {return _taskID;}
            time_t timeUpdated() const  {return _timeUpdated;}
            virtual bool finished() const =0;
            virtual void writeDescription(fleece::JSONEncoder&);

            virtual void stop() =0;

            void registerTask();
            void unregisterTask();

        protected:
            virtual ~Task() =default;

            time_t _timeUpdated {0};
        private:
            RESTListener* const _listener;
            unsigned _taskID {0};
            time_t _timeStarted {0};
        };

        /** The currently-running tasks. */
        std::vector<Retained<Task>> tasks();

    protected:
        friend class Task;

        Retained<net::TLSContext> createTLSContext(const C4TLSConfig*);
        Retained<crypto::Identity> loadTLSIdentity(const C4TLSConfig*);
        
        Server* server() const              {return _server.get();}

        /** Returns the database for this request, or null on error. */
        Retained<C4Database> databaseFor(RequestResponse&);
        unsigned registerTask(Task*);
        void unregisterTask(Task*);

        using HandlerMethod = void(RESTListener::*)(RequestResponse&);
        using DBHandlerMethod = void(RESTListener::*)(RequestResponse&, C4Database*);

        void addHandler(net::Method, const char *uri, HandlerMethod);
        void addDBHandler(net::Method, const char *uri, DBHandlerMethod);
        
        std::vector<net::Address> _addresses(C4Database *dbOrNull =nullptr,
                                            C4ListenerAPIs api = kC4RESTAPI) const;

        virtual void handleSync(RequestResponse&, C4Database*);

        static std::string serverNameAndVersion();
        static std::string kServerName;

    private:
        void handleGetRoot(RequestResponse&);
        void handleGetAllDBs(RequestResponse&);
        void handleReplicate(RequestResponse&);
        void handleActiveTasks(RequestResponse&);

        void handleGetDatabase(RequestResponse&, C4Database*);
        void handleCreateDatabase(RequestResponse&);
        void handleDeleteDatabase(RequestResponse&, C4Database*);

        void handleGetAllDocs(RequestResponse&, C4Database*);
        void handleGetDoc(RequestResponse&, C4Database*);
        void handleModifyDoc(RequestResponse&, C4Database*);
        void handleBulkDocs(RequestResponse&, C4Database*);

        bool modifyDoc(fleece::Dict body,
                       std::string docID,
                       std::string revIDQuery,
                       bool deleting,
                       bool newEdits,
                       C4Database *db,
                       fleece::JSONEncoder& json,
                       C4Error *outError) noexcept;

        std::unique_ptr<FilePath> _directory;
        const bool _allowCreateDB, _allowDeleteDB;
        Retained<crypto::Identity> _identity;
        Retained<Server> _server;
        std::set<Retained<Task>> _tasks;
        unsigned _nextTaskID {1};
    };

} }
