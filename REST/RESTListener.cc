//
// RESTListener.cc
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
// <https://github.com/civetweb/civetweb/blob/master/docs/UserManual.md#configuration-options>

#include "RESTListener.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "Server.hh"
#include "Request.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <functional>
#include <queue>

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace REST {

    static constexpr const char* kKeepAliveTimeoutMS = "1000";
    static constexpr const char* kMaxConnections = "8";

    static int kTaskExpirationTime = 10;


    RESTListener::RESTListener(const Config &config)
    :_directory(config.directory.buf ? new FilePath(slice(config.directory).asString(), "")
                                     : nullptr)
    ,_allowCreateDB(config.allowCreateDBs && _directory)
    ,_allowDeleteDB(config.allowDeleteDBs)
    {
        auto portStr = to_string(config.port ? config.port : kDefaultPort);
        const char* options[] {
            "listening_ports",          portStr.c_str(),
            "enable_keep_alive",        "yes",
            "keep_alive_timeout_ms",    kKeepAliveTimeoutMS,
            "num_threads",              kMaxConnections,
            "decode_url",               "no",   // otherwise it decodes escaped slashes
            nullptr
        };
        _server.reset(new Server(options, this));
        _server->setExtraHeaders({{"Server", "LiteCoreServ/0.0"}});

        if (config.apis & kC4RESTAPI) {
            auto notFound =  [](RequestResponse &rq) {
                rq.respondWithStatus(HTTPStatus::NotFound, "Not Found");
            };

            // Root:
            addHandler(Server::GET, "/$", &RESTListener::handleGetRoot);

            // Top-level special handlers:
            addHandler(Server::GET, "/_all_dbs$",       &RESTListener::handleGetAllDBs);
            addHandler(Server::GET, "/_active_tasks$",  &RESTListener::handleActiveTasks);
            addHandler(Server::POST, "/_replicate$",    &RESTListener::handleReplicate);
            _server->addHandler(Server::DEFAULT, "/_",  notFound);

            // Database:
            addDBHandler(Server::GET,   "/*$|/*/$", &RESTListener::handleGetDatabase);
            addHandler  (Server::PUT,   "/*$|/*/$", &RESTListener::handleCreateDatabase);
            addDBHandler(Server::DELETE,"/*$|/*/$", &RESTListener::handleDeleteDatabase);
            addDBHandler(Server::POST,  "/*$|/*/$", &RESTListener::handleModifyDoc);

            // Database-level special handlers:
            addDBHandler(Server::GET, "/*/_all_docs$", &RESTListener::handleGetAllDocs);
            addDBHandler(Server::POST, "/*/_bulk_docs$", &RESTListener::handleBulkDocs);
            _server->addHandler(Server::DEFAULT, "/*/_", notFound);

            // Document:
            addDBHandler(Server::GET,   "/*/*$", &RESTListener::handleGetDoc);
            addDBHandler(Server::PUT,   "/*/*$", &RESTListener::handleModifyDoc);
            addDBHandler(Server::DELETE,"/*/*$", &RESTListener::handleModifyDoc);
        }
    }


    RESTListener::~RESTListener() {
    }


#pragma mark - REGISTERING DATABASES:


    static void replace(string &str, char oldChar, char newChar) {
        for (char &c : str)
            if (c == oldChar)
                c = newChar;
    }


    static bool returnError(C4Error* outError,
                            C4ErrorDomain domain, int code, const char *message =nullptr)
    {
        if (outError)
            *outError = c4error_make(domain, code, c4str(message));
        return false;
    }


    bool RESTListener::pathFromDatabaseName(const string &name, FilePath &path) {
        if (!_directory || !isValidDatabaseName(name))
            return false;
        string filename = name;
        replace(filename, '/', ':');
        path = (*_directory)[filename + kC4DatabaseFilenameExtension + "/"];
        return true;
    }


    bool RESTListener::openDatabase(std::string name,
                                const FilePath &path,
                                const C4DatabaseConfig *config,
                                C4Error *outError)
    {
        if (name.empty()) {
            name = databaseNameFromPath(path);
            if (name.empty())
                return returnError(outError, LiteCoreDomain, kC4ErrorInvalidParameter,
                                   "Invalid database name");
        }
        if (databaseNamed(name) != nullptr)
            return returnError(outError, LiteCoreDomain, kC4ErrorConflict, "Database exists");
        c4::ref<C4Database> db = c4db_open(slice(path.path()), config, outError);
        if (!db)
            return false;
        if (!registerDatabase(name, db)) {
            //FIX: If db didn't exist before the c4db_open call, should delete it
            return returnError(outError, LiteCoreDomain, kC4ErrorConflict, "Database exists");
        }
        return db;
    }


#pragma mark - TASKS:


    void RESTListener::Task::registerTask() {
        if (!_taskID) {
            time(&_timeStarted);
            _taskID = _listener->registerTask(this);
        }
    }


    void RESTListener::Task::unregisterTask() {
        if (_taskID) {
            _listener->unregisterTask(this);
            _taskID = 0;
        }
    }



    void RESTListener::Task::writeDescription(fleeceapi::JSONEncoder &json) {
        json.writeKey("pid"_sl);
        json.writeUInt(_taskID);
        json.writeKey("started_on"_sl);
        json.writeUInt(_timeStarted);
    }


    unsigned RESTListener::registerTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.insert(task);
        return _nextTaskID++;
    }


    void RESTListener::unregisterTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.erase(task);
    }


    vector<Retained<RESTListener::Task>> RESTListener::tasks() {
        lock_guard<mutex> lock(_mutex);

        // Clean up old finished tasks:
        time_t now;
        time(&now);
        for (auto i = _tasks.begin(); i != _tasks.end(); ) {
            if ((*i)->finished() && (now - (*i)->timeUpdated()) >= kTaskExpirationTime)
                i = _tasks.erase(i);
            else
                ++i;
        }

        return vector<Retained<Task>>(_tasks.begin(), _tasks.end());
    }


#pragma mark - UTILITIES:


    void RESTListener::addHandler(Server::Method method, const char *uri, HandlerMethod handler) {
        using namespace std::placeholders;
        _server->addHandler(method, uri, bind(handler, this, _1));
    }

    void RESTListener::addDBHandler(Server::Method method, const char *uri, DBHandlerMethod handler) {
        _server->addHandler(method, uri, [this,handler](RequestResponse &rq) {
            c4::ref<C4Database> db = databaseFor(rq);
            if (db) {
                c4db_lock(db);
                try {
                    (this->*handler)(rq, db);
                } catch (...) {
                    c4db_unlock(db);
                    throw;
                }
                c4db_unlock(db);
            }
        });
    }

    
    c4::ref<C4Database> RESTListener::databaseFor(RequestResponse &rq) {
        string dbName = rq.path(0);
        if (dbName.empty()) {
            rq.respondWithStatus(HTTPStatus::BadRequest);
            return nullptr;
        }
        auto db = databaseNamed(dbName);
        if (!db)
            rq.respondWithStatus(HTTPStatus::NotFound);
        return db;
    }

    

} }
