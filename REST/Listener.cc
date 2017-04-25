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

        auto notFound =  [](Request &rq) { rq.respondWithError(404, "Not Found"); };

        // Root:
        _server->addHandler(Server::GET, "/$", [=](Request &rq) { handleGetRoot(rq); });

        // Top-level special handlers:
        _server->addHandler(Server::GET, "/_all_dbs$", [=](Request &rq) { handleGetAllDBs(rq); });
        _server->addHandler(Server::DEFAULT, "/_", notFound);

        // Database:
        addDBHandler(Server::GET,   "/*$|/*/$", &Listener::handleGetDatabase);
        _server->addHandler(Server::PUT,   "/*$|/*/$", [this](Request &rq) {handleCreateDatabase(rq);});
        addDBHandler(Server::DELETE,"/*$|/*/$", &Listener::handleDeleteDatabase);
        addDBHandler(Server::POST,  "/*$|/*/$", &Listener::handleModifyDoc);

        // Database-level special handlers:
        addDBHandler(Server::GET, "/*/_all_docs$", &Listener::handleGetAllDocs);
        _server->addHandler(Server::DEFAULT, "/*/_", notFound);

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


#pragma mark - UTILITIES:


    void Listener::addDBHandler(Server::Method method, const char *uri, DBHandler handler) {
        _server->addHandler(method, uri, [this,handler](Request &rq) {
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

    
    c4::ref<C4Database> Listener::databaseFor(Request &rq) {
        string dbName = rq.path(0);
        if (dbName.empty()) {
            rq.respondWithError(400);
            return nullptr;
        }
        auto db = databaseNamed(dbName);
        if (!db)
            rq.respondWithError(404);
        return db;
    }

    
#pragma mark - ROOT / DATABASE HANDLERS:


    void Listener::handleGetRoot(Request &rq) {
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("couchdb"_sl);
        json.writeString("Welcome"_sl);
        json.writeKey("vendor"_sl);
        json.beginDict();
        json.writeKey("name"_sl);
        json.writeString("LiteCoreServ"_sl);
        json.writeKey("version"_sl);
        json.writeString("0.0"_sl);
        json.endDict();
        json.writeKey("version"_sl);
        json.writeString("LiteCoreServ/0.0"_sl);
        json.endDict();
    }


    void Listener::handleGetAllDBs(Request &rq) {
        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (string &name : databaseNames())
            json.writeString(name);
        json.endArray();
    }


    void Listener::handleGetDatabase(Request &rq, C4Database *db) {
        auto docCount = c4db_getDocumentCount(db);
        auto lastSequence = c4db_getLastSequence(db);
        C4UUID uuid;
        c4db_getUUIDs(db, &uuid, nullptr, nullptr);
        auto uuidStr = slice(&uuid, sizeof(uuid)).hexString();

        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("db_name"_sl);
        json.writeString(rq.path(0));
        json.writeKey("db_uuid"_sl);
        json.writeString(uuidStr);
        json.writeKey("doc_count"_sl);
        json.writeUInt(docCount);
        json.writeKey("update_seq"_sl);
        json.writeUInt(lastSequence);
        json.writeKey("committed_update_seq"_sl);
        json.writeUInt(lastSequence);
        json.endDict();
    }


    void Listener::handleCreateDatabase(Request &rq) {
        if (!_allowCreateDB)
            return rq.respondWithError(403, "Cannot create databases");
        string dbName = rq.path(0);
        if (databaseNamed(dbName))
            return rq.respondWithError(412, "Database exists");
        FilePath path;
        if (!pathFromDatabaseName(dbName, path))
            return rq.respondWithError(400, "Invalid database name");

        C4DatabaseConfig config = { kC4DB_Bundled | kC4DB_SharedKeys | kC4DB_Create };
        C4Error err;
        if (!openDatabase(dbName, path, &config, &err)) {
            if (err.domain == LiteCoreDomain && err.code == kC4ErrorConflict)
                return rq.respondWithError(412);
            else
                return rq.respondWithError(err);
        }
        rq.setStatus(201, "Created");
    }


    void Listener::handleDeleteDatabase(Request &rq, C4Database *db) {
        if (!_allowDeleteDB)
            return rq.respondWithError(403, "Cannot delete databases");
        string name = rq.path(0);
        if (!unregisterDatabase(name))
            return rq.respondWithError(404);
        C4Error err;
        if (!c4db_delete(db, &err)) {
            registerDatabase(name, db);
            return rq.respondWithError(err);
        }
    }


#pragma mark - DOCUMENT HANDLERS:


    void Listener::handleGetAllDocs(Request &rq, C4Database *db) {
        // Apply options:
        C4EnumeratorOptions options;
        options.flags = kC4InclusiveStart | kC4InclusiveEnd | kC4IncludeNonConflicted;
        options.skip = 0;
        if (rq.boolQuery("descending"))
            options.flags |= kC4Descending;
        bool includeDocs = rq.boolQuery("include_docs");
        if (includeDocs)
            options.flags |= kC4IncludeBodies;
        // TODO: Implement startkey, endkey, skip, limit, etc.

        // Create enumerator:
        C4Error err;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &err);
        if (!e)
            return rq.respondWithError(err);

        // Enumerate, building JSON:
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("rows"_sl);
        json.beginArray();
        while (c4enum_next(e, &err)) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            json.beginDict();
            json.writeKey("key"_sl);
            json.writeString(info.docID);
            json.writeKey("id"_sl);
            json.writeString(info.docID);
            json.writeKey("value"_sl);
            json.beginDict();
            json.writeKey("rev"_sl);
            json.writeString(info.revID);
            json.endDict();

            if (includeDocs) {
                c4::ref<C4Document> doc = c4enum_getDocument(e, &err);
                if (!doc)
                    return rq.respondWithError(err);
                alloc_slice docBody = c4doc_bodyAsJSON(doc, &err);
                if (!docBody)
                    return rq.respondWithError(err);
                json.writeKey("doc"_sl);
                json.writeRaw(docBody);
            }
            json.endDict();
        }
        json.endArray();
        json.endDict();
    }


    void Listener::handleGetDoc(Request &rq, C4Database *db) {
        string docID = rq.path(1);
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(db, slice(docID), true, &err);
        if (!doc)
            return rq.respondWithError(err);

        string revID = rq.query("rev");
        if (revID.empty()) {
            if (doc->flags & kDeleted)
                return rq.respondWithError(404);
            revID = slice(doc->revID).asString();
        } else {
            if (!c4doc_selectRevision(doc, slice(revID), true, &err))
                return rq.respondWithError(err);
        }

        // Get the revision
        if (!doc->selectedRev.body.buf)
            return rq.respondWithError(404);
        alloc_slice json = c4doc_bodyAsJSON(doc, &err);
        if (!json)
            return rq.respondWithError(err);

        // Splice the _id and _rev into the start of the JSON:
        rq.setHeader("Content-Type", "application/json");
        rq.setChunked();    // OPT: Buffer this and write in one go
        rq.write("{\"_id\":\"");
        rq.write(docID);
        rq.write("\",\"_rev\":\"");
        rq.write(revID);
        if (doc->selectedRev.flags & kRevDeleted)
            rq.write("\",\"_deleted\":true");
        if (json.size > 2) {
            rq.write("\",");
            slice suffix = json;
            suffix.moveStart(1);
            rq.write(suffix);
        } else {
            rq.write("}");
        }
    }


    // This handles PUT and DELETE of a document, as well as POST to a database.
    void Listener::handleModifyDoc(Request &rq, C4Database *db) {
        string docID = rq.path(1);                       // will be empty for POST

        // Parse the body:
        bool deleting = (rq.method() == "DELETE"_sl);
        Dict body = rq.bodyAsJSON().asDict();
        if (!body) {
            if (!deleting || rq.body())
                return rq.respondWithError(400);
        }

        // Get the revID from either the JSON body or the "rev" query param:
        slice revID = body["_rev"_sl].asString();
        string revIDQuery = rq.query("rev");
        if (!revIDQuery.empty()) {
            if (revID) {
                if (revID != slice(revIDQuery))
                    return rq.respondWithError(400);
            } else {
                revID = slice(revIDQuery);
            }
        }

        if (docID.empty()) {
            if (revID)
                return rq.respondWithError(400);            // Can't specify revID on a POST
            docID = slice(body["_id"].asString()).asString();
        }

        if (body["_deleted"_sl].asBool())
            deleting = true;

        // Encode body as Fleece (and strip _id and _rev):
        alloc_slice encodedBody = c4doc_encodeStrippingOldMetaProperties(body);

        // Save the revision:
        C4Slice history[1] = {revID};
        C4DocPutRequest put = {};
        put.body = encodedBody;
        if (!docID.empty())
            put.docID = slice(docID);
        put.revFlags = (deleting ? kRevDeleted : 0);
        put.existingRevision = false;
        put.allowConflict = false;
        put.history = history;
        put.historyCount = revID ? 1 : 0;
        put.save = true;

        c4::ref<C4Document> doc;
        {
            C4Error err;
            c4::Transaction t(db);
            if (t.begin(&err))
                doc = c4doc_put(db, &put, nullptr, &err);
            if (!doc || !t.commit(&err))
                return rq.respondWithError(err);
        }
        revID = slice(doc->selectedRev.revID);

        // Return a JSON object:
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("ok"_sl);
        json.writeBool(true);
        json.writeKey("id"_sl);
        json.writeString(doc->docID);
        json.writeKey("rev"_sl);
        json.writeString(doc->selectedRev.revID);
        json.endDict();

        if (deleting)
            rq.setStatus(200, "Deleted");
        else
            rq.setStatus(201, "Created");
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
