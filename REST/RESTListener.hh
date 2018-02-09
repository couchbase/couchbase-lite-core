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
#include "c4.hh"
#include "c4Listener.h"
#include "Listener.hh"
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

    class RESTListener : public Listener {
    public:
        RESTListener(const Config&);
        ~RESTListener();

        bool pathFromDatabaseName(const std::string &name, FilePath &outPath);

        /** Opens a database and makes it visible via the REST API.
            If the name is an empty string, a default name will be used based on the
            filename. */
        bool openDatabase(std::string name,
                          const FilePath&,
                          const C4DatabaseConfig*,
                          C4Error*);

        /** An asynchronous task (like a replication). */
        class Task : public RefCounted {
        public:
            explicit Task(RESTListener* listener)    :_listener(listener) { }

            RESTListener* listener() const  {return _listener;}
            unsigned taskID() const     {return _taskID;}
            time_t timeUpdated() const  {return _timeUpdated;}
            virtual bool finished() const =0;
            virtual void writeDescription(fleeceapi::JSONEncoder&);

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

        std::vector<Retained<Task>> tasks();

    protected:
        friend class Task;

        Server* server() const              {return _server.get();}

        /** Returns the database for this request, or null on error. */
        c4::ref<C4Database> databaseFor(RequestResponse&);
        unsigned registerTask(Task*);
        void unregisterTask(Task*);

        using HandlerMethod = void(RESTListener::*)(RequestResponse&);
        using DBHandlerMethod = void(RESTListener::*)(RequestResponse&, C4Database*);

        void addHandler(Server::Method, const char *uri, HandlerMethod);
        void addDBHandler(Server::Method, const char *uri, DBHandlerMethod);

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

        bool modifyDoc(fleeceapi::Dict body,
                       std::string docID,
                       std::string revIDQuery,
                       bool deleting,
                       bool newEdits,
                       C4Database *db,
                       fleeceapi::JSONEncoder& json,
                       C4Error *outError);

        std::unique_ptr<FilePath> _directory;
        const bool _allowCreateDB, _allowDeleteDB;
        std::unique_ptr<Server> _server;
        std::mutex _mutex;
        std::set<Retained<Task>> _tasks;
        unsigned _nextTaskID {1};
    };

} }
