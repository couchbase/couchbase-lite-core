//
// RESTListener.hh
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
#include "c4Listener.hh"
#include "Listener.hh"
#include "Server.hh"
#include "FilePath.hh"
#include "fleece/RefCounted.hh"
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

        Retained<C4Database> getDatabase(RequestResponse &rq, const std::string &dbName);

        /** Returns the collection for this request, or null on error */
        std::pair<Retained<C4Database>,C4Collection*> collectionFor(RequestResponse&);
        unsigned registerTask(Task*);
        void unregisterTask(Task*);

        using HandlerMethod = void(RESTListener::*)(RequestResponse&);
        using DBHandlerMethod = void(RESTListener::*)(RequestResponse&, C4Database*);
        using CollectionHandlerMethod = void(RESTListener::*)(RequestResponse&, C4Collection*);

        void addHandler(net::Method, const char *uri, HandlerMethod);
        void addDBHandler(net::Method, const char *uri, DBHandlerMethod);
        void addCollectionHandler(net::Method, const char *uri, CollectionHandlerMethod);
        
        std::vector<net::Address> _addresses(C4Database *dbOrNull =nullptr,
                                            C4ListenerAPIs api = kC4RESTAPI) const;

        virtual void handleSync(RequestResponse&, C4Database*);

        static std::string serverNameAndVersion();
        static std::string kServerName;

    private:
        std::pair<std::string,C4CollectionSpec> parseKeySpace(slice keySpace);
        bool collectionGiven(RequestResponse&);

        void handleGetRoot(RequestResponse&);
        void handleGetAllDBs(RequestResponse&);
        void handleReplicate(RequestResponse&);
        void handleActiveTasks(RequestResponse&);

        void handleGetDatabase(RequestResponse&, C4Collection*);
        void handleCreateDatabase(RequestResponse&);
        void handleDeleteDatabase(RequestResponse&, C4Collection*);

        void handleGetAllDocs(RequestResponse&, C4Collection*);
        void handleGetDoc(RequestResponse&, C4Collection*);
        void handleModifyDoc(RequestResponse&, C4Collection*);
        void handleBulkDocs(RequestResponse&, C4Collection*);

        bool modifyDoc(fleece::Dict body,
                       std::string docID,
                       std::string revIDQuery,
                       bool deleting,
                       bool newEdits,
                       C4Collection *coll,
                       fleece::JSONEncoder& json,
                       C4Error *outError) noexcept;

        std::unique_ptr<FilePath> _directory;
        const bool _allowCreateDB, _allowDeleteDB, _allowCreateCollection, _allowDeleteCollection;
        Retained<crypto::Identity> _identity;
        Retained<Server> _server;
        std::set<Retained<Task>> _tasks;
        unsigned _nextTaskID {1};
    };

} }
