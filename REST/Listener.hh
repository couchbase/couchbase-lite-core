//
//  Listener.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "c4REST.h"
#include "Server.hh"
#include "FilePath.hh"
#include "RefCounted.hh"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace litecore { namespace REST {
    class RequestResponse;
    class Request;
    class Server;

    class Listener {
    public:
        using Config = C4RESTConfig;

        Listener(const Config&);
        ~Listener();

        static bool isValidDatabaseName(const std::string&);
        static std::string databaseNameFromPath(const FilePath&);
        bool pathFromDatabaseName(const std::string &name, FilePath &outPath);

        /** Makes a database visible via the REST API. */
        bool registerDatabase(std::string name, C4Database*);

        /** Unregisters a database by name. 
            The C4Database will be closed if there are no other references to it. */
        bool unregisterDatabase(std::string name);

        /** Opens a database and makes it visible via the REST API.
            If the name is an empty string, a default name will be used based on the
            filename. */
        bool openDatabase(std::string name,
                          const FilePath&,
                          const C4DatabaseConfig*,
                          C4Error*);

        /** Returns the database registered under the given name. */
        c4::ref<C4Database> databaseNamed(const std::string &name);

        std::vector<std::string> databaseNames();


        class Task : public RefCounted {
        public:
            Task(Listener* listener)    :_listener(listener) { }
            unsigned taskID() const     {return _taskID;}
            time_t timeUpdated() const  {return _timeUpdated;}
            virtual bool finished() const =0;
            virtual void writeDescription(fleeceapi::JSONEncoder&);

            void registerTask();
            void unregisterTask();

        protected:
            time_t _timeUpdated {0};
        private:
            Listener* const _listener;
            unsigned _taskID {0};
            time_t _timeStarted {0};
        };

        std::vector<Retained<Task>> tasks();

    private:
        friend class Task;

        /** Returns the database for this request, or null on error. */
        c4::ref<C4Database> databaseFor(RequestResponse&);
        unsigned registerTask(Task*);
        void unregisterTask(Task*);

        using HandlerMethod = void(Listener::*)(RequestResponse&);
        using DBHandlerMethod = void(Listener::*)(RequestResponse&, C4Database*);

        void addHandler(Server::Method, const char *uri, HandlerMethod);
        void addDBHandler(Server::Method, const char *uri, DBHandlerMethod);

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

        std::unique_ptr<FilePath> _directory;
        const bool _allowCreateDB, _allowDeleteDB;
        std::unique_ptr<Server> _server;
        std::mutex _mutex;
        std::map<std::string, c4::ref<C4Database>> _databases;
        std::set<Retained<Task>> _tasks;
        unsigned _nextTaskID {1};
    };

} }
