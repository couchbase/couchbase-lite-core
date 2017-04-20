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
#include "c4REST.h"
#include "c4Document+Fleece.h"
#include "Server.hh"
#include "Request.hh"
#include "StringUtil.hh"
#include <functional>
#include <queue>

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace REST {

#define kKeepAliveTimeoutMS     "1000"
#define kMaxConnections         "8"


    Listener::Listener(uint16_t port) {
        auto portStr = to_string(port);
        const char* options[] {
            "listening_ports",          portStr.c_str(),
            "enable_keep_alive",        "yes",
            "keep_alive_timeout_ms",    kKeepAliveTimeoutMS,
            "num_threads",              kMaxConnections,
            nullptr
        };
        _server.reset(new Server(options, this));
        _server->setExtraHeaders({{"Server", "LiteCoreServ/0.0"}});

        auto notFound =  [](Request &rq) { rq.respondWithError(404, "Not Found"); };

        // Root:
        _server->addHandler(Server::GET, "/$", [](Request &rq) { handleGetRoot(rq); });

        // Top-level special handlers:
        _server->addHandler(Server::GET, "/_all_dbs$", [](Request &rq) { handleGetAllDBs(rq); });
        _server->addHandler(Server::DEFAULT, "/_", notFound);

        // Database:
        _server->addHandler(Server::GET,  "/*$|/*/$", [](Request &rq) { handleGetDatabase(rq); });
        _server->addHandler(Server::POST, "/*$|/*/$", [](Request &rq) { handleModifyDoc(rq); });

        // Database-level special handlers:
        _server->addHandler(Server::GET, "/*/_all_docs$", [](Request &rq) { handleGetAllDocs(rq); });
        _server->addHandler(Server::DEFAULT, "/*/_", notFound);

        // Document:
        _server->addHandler(Server::GET,   "/*/*$", [](Request &rq) { handleGetDoc(rq); });
        _server->addHandler(Server::PUT,   "/*/*$", [](Request &rq) { handleModifyDoc(rq); });
        _server->addHandler(Server::DELETE,"/*/*$", [](Request &rq) { handleModifyDoc(rq); });
    }


    Listener::~Listener() {
    }


    void Listener::registerDatabase(string name, C4Database *db) {
        lock_guard<mutex> lock(_mutex);
        _databases[name] = c4db_retain(db);
    }


    C4Database* Listener::databaseNamed(const string &name) {
        lock_guard<mutex> lock(_mutex);
        auto i = _databases.find(name);
        if (i == _databases.end())
            return nullptr;
        return i->second;
    }


    vector<string> Listener::databaseNames() {
        lock_guard<mutex> lock(_mutex);
        vector<string> names;
        for (auto &d : _databases)
            names.push_back(d.first);
        return names;
    }


#pragma mark - UTILITIES:


    static Listener& listener(Request &rq) {
        return *(Listener*)rq.server()->owner();
    }

    static C4Database* database(Request &rq) {
        auto dbName = rq.path(0);
        if (!dbName) {
            rq.respondWithError(400);
            return nullptr;
        }
        auto db = listener(rq).databaseNamed((string)dbName);
        if (!db)
            rq.respondWithError(404);
        return db;
    }

    
#pragma mark - HANDLERS:


    void Listener::handleGetRoot(Request &rq) {
        auto &json = rq.json();
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
        auto &json = rq.json();
        json.beginArray();
        for (string &name : listener(rq).databaseNames())
            json.writeString(name);
        json.endArray();
    }


    void Listener::handleGetDatabase(Request &rq) {
        C4Database *db = database(rq);
        if (!db)
            return;

        auto docCount = c4db_getDocumentCount(db);
        auto lastSequence = c4db_getLastSequence(db);
        C4UUID uuid;
        c4db_getUUIDs(db, &uuid, nullptr, nullptr);
        auto uuidStr = slice(&uuid, sizeof(uuid)).hexString();

        auto &json = rq.json();
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


    void Listener::handleGetAllDocs(Request &rq) {
        C4Database *db = database(rq);
        if (!db)
            return;

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
        auto &json = rq.json();
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


    void Listener::handleGetDoc(Request &rq) {
        C4Database *db = database(rq);
        if (!db)
            return;
        slice docID = rq.path(1);
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(db, docID, true, &err);
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


    void Listener::handleModifyDoc(Request &rq) {
        C4Database *db = database(rq);
        if (!db)
            return;
        slice docID = rq.path(1);                       // will be null for POST

        // Parse the body:
        bool deleting = (rq.method() == "DELETE"_sl);
        Dict body = rq.requestJSON().asDict();
        if (!body) {
            if (!deleting || rq.requestBody())
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

        if (!docID) {
            if (revID)
                return rq.respondWithError(400);            // Can't specify revID on a POST
            docID = slice(body["_id"].asString());
        }

        if (body["_deleted"_sl].asBool())
            deleting = true;

        // Encode body as Fleece (and strip _id and _rev):
        alloc_slice encodedBody = c4doc_encodeStrippingOldMetaProperties(body);

        // Save the revision:
        C4Slice history[1] = {revID};
        C4DocPutRequest put = {};
        put.body = encodedBody;
        put.docID = docID;
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
        auto &json = rq.json();
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

using namespace litecore::REST;

static inline Listener* internal(C4RESTListener* r) {return (Listener*)r;}
static inline C4RESTListener* external(Listener* r) {return (C4RESTListener*)r;}

C4RESTListener* c4rest_start(uint16_t port, C4Error *error) noexcept {
    return external(new Listener(port));
}

void c4rest_free(C4RESTListener *listener) noexcept {
    delete internal(listener);
}

void c4rest_shareDB(C4RESTListener *listener, C4String name, C4Database *db) noexcept {
    internal(listener)->registerDatabase(slice(name).asString(), db);
}
