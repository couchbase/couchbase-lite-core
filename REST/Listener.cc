//
//  Listener.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//
// <https://github.com/civetweb/civetweb/blob/master/docs/UserManual.md#configuration-options>

#include "Listener.hh"
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

    static constexpr uint16_t kDefaultPort = 4984;

    static constexpr const char* kKeepAliveTimeoutMS = "1000";
    static constexpr const char* kMaxConnections = "8";

    static int kTaskExpirationTime = 10;


    Listener::Listener(const Config &config)
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

        auto notFound =  [](RequestResponse &rq) {
            rq.respondWithStatus(HTTPStatus::NotFound, "Not Found");
        };

        // Root:
        addHandler(Server::GET, "/$", &Listener::handleGetRoot);

        // Top-level special handlers:
        addHandler(Server::GET, "/_all_dbs$",       &Listener::handleGetAllDBs);
        addHandler(Server::GET, "/_active_tasks$",  &Listener::handleActiveTasks);
        addHandler(Server::POST, "/_replicate$",    &Listener::handleReplicate);
        _server->addHandler(Server::DEFAULT, "/_",  notFound);

        // Database:
        addDBHandler(Server::GET,   "/*$|/*/$", &Listener::handleGetDatabase);
        addHandler  (Server::PUT,   "/*$|/*/$", &Listener::handleCreateDatabase);
        addDBHandler(Server::DELETE,"/*$|/*/$", &Listener::handleDeleteDatabase);
        addDBHandler(Server::POST,  "/*$|/*/$", &Listener::handleModifyDoc);

        // Database-level special handlers:
        addDBHandler(Server::GET, "/*/_all_docs$", &Listener::handleGetAllDocs);
        addDBHandler(Server::POST, "/*/_bulk_docs$", &Listener::handleBulkDocs);
        _server->addHandler  (Server::DEFAULT, "/*/_", notFound);

        // Document:
        addDBHandler(Server::GET,   "/*/*$", &Listener::handleGetDoc);
        addDBHandler(Server::PUT,   "/*/*$", &Listener::handleModifyDoc);
        addDBHandler(Server::DELETE,"/*/*$", &Listener::handleModifyDoc);
    }


    Listener::~Listener() {
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


    string Listener::databaseNameFromPath(const FilePath &path) {
        string name = path.fileOrDirName();
        auto split = FilePath::splitExtension(name);
        if (split.second != kC4DatabaseFilenameExtension)
            return string();
        name = split.first;
        replace(name, ':', '/');
        if (!isValidDatabaseName(name))
            return string();
        return name;
    }


    bool Listener::pathFromDatabaseName(const string &name, FilePath &path) {
        if (!_directory || !isValidDatabaseName(name))
            return false;
        string filename = name;
        replace(filename, '/', ':');
        path = (*_directory)[filename + kC4DatabaseFilenameExtension + "/"];
        return true;
    }


    bool Listener::isValidDatabaseName(const string &name) {
        // Same rules as Couchbase Lite 1.x and CouchDB
        return name.size() > 0 && name.size() < 240
            && islower(name[0])
            && !slice(name).findByteNotIn("abcdefghijklmnopqrstuvwxyz0123456789_$()+-/"_sl);
    }


    bool Listener::openDatabase(std::string name,
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


    bool Listener::registerDatabase(string name, C4Database *db) {
        if (!isValidDatabaseName(name))
            return false;
        lock_guard<mutex> lock(_mutex);
        if (_databases.find(name) != _databases.end())
            return false;
        _databases.emplace(name, c4db_retain(db));
        return true;
    }


    bool Listener::unregisterDatabase(std::string name) {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return false;
        _databases.erase(i);
        return true;
    }


    c4::ref<C4Database> Listener::databaseNamed(const string &name) {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return nullptr;
        //FIX: Prevent multiple handlers from accessing the same db at once. Need a mutex per db, I think.

        // Retain the database to avoid a race condition if it gets unregistered while this
        // thread's handler is still using it.
        return c4::ref<C4Database>(c4db_retain(i->second));
    }


    vector<string> Listener::databaseNames() {
        lock_guard<mutex> lock(_mutex);
        vector<string> names;
        for (auto &d : _databases)
            names.push_back(d.first);
        return names;
    }


#pragma mark - TASKS:


    void Listener::Task::registerTask() {
        if (!_taskID) {
            time(&_timeStarted);
            _taskID = _listener->registerTask(this);
        }
    }


    void Listener::Task::unregisterTask() {
        if (_taskID) {
            _listener->unregisterTask(this);
            _taskID = 0;
        }
    }



    void Listener::Task::writeDescription(fleeceapi::JSONEncoder &json) {
        json.writeKey("pid"_sl);
        json.writeUInt(_taskID);
        json.writeKey("started_on"_sl);
        json.writeUInt(_timeStarted);
    }


    unsigned Listener::registerTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.insert(task);
        return _nextTaskID++;
    }


    void Listener::unregisterTask(Task *task) {
        lock_guard<mutex> lock(_mutex);
        _tasks.erase(task);
    }


    vector<Retained<Listener::Task>> Listener::tasks() {
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


    void Listener::addHandler(Server::Method method, const char *uri, HandlerMethod handler) {
        using namespace std::placeholders;
        _server->addHandler(method, uri, bind(handler, this, _1));
    }

    void Listener::addDBHandler(Server::Method method, const char *uri, DBHandlerMethod handler) {
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

    
    c4::ref<C4Database> Listener::databaseFor(RequestResponse &rq) {
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


#pragma mark - C API:

using namespace litecore;
using namespace litecore::REST;

const char* const kC4DatabaseFilenameExtension = ".cblite2";

static inline Listener* internal(C4RESTListener* r) {return (Listener*)r;}
static inline C4RESTListener* external(Listener* r) {return (C4RESTListener*)r;}

C4RESTListener* c4rest_start(C4RESTConfig *config, C4Error *error) noexcept {
    try {
        return external(new Listener(*config));
    } catchExceptions()
    return nullptr;
}

void c4rest_free(C4RESTListener *listener) noexcept {
    delete internal(listener);
}


C4StringResult c4rest_databaseNameFromPath(C4String pathSlice) noexcept {
    try {
        auto pathStr = slice(pathSlice).asString();
        string name = Listener::databaseNameFromPath(FilePath(pathStr, ""));
        if (name.empty())
            return {};
        alloc_slice result(name);
        result.retain();
        return {(char*)result.buf, result.size};
    } catchExceptions()
    return {};
}

void c4rest_shareDB(C4RESTListener *listener, C4String name, C4Database *db) noexcept {
    try {
        internal(listener)->registerDatabase(slice(name).asString(), db);
    } catchExceptions()
}
