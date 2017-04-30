
//  Listener+Handlers.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/27/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Listener.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "Server.hh"
#include "Request.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <functional>

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace REST {

#pragma mark - ROOT HANDLERS:


    void Listener::handleGetRoot(RequestResponse &rq) {
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


    void Listener::handleGetAllDBs(RequestResponse &rq) {
        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (string &name : databaseNames())
            json.writeString(name);
        json.endArray();
    }


    void Listener::handleActiveTasks(RequestResponse &rq) {
        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (auto &task : tasks()) {
            json.beginDict();
            task->writeDescription(json);
            json.endDict();
        }
        json.endArray();
    }


#pragma mark - DATABASE HANDLERS:

    
    void Listener::handleGetDatabase(RequestResponse &rq, C4Database *db) {
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


    void Listener::handleCreateDatabase(RequestResponse &rq) {
        if (!_allowCreateDB)
            return rq.respondWithError(HTTPStatus::Forbidden, "Cannot create databases");
        string dbName = rq.path(0);
        if (databaseNamed(dbName))
            return rq.respondWithError(HTTPStatus::PreconditionFailed, "Database exists");
        FilePath path;
        if (!pathFromDatabaseName(dbName, path))
            return rq.respondWithError(HTTPStatus::BadRequest, "Invalid database name");

        C4DatabaseConfig config = { kC4DB_Bundled | kC4DB_SharedKeys | kC4DB_Create };
        C4Error err;
        if (!openDatabase(dbName, path, &config, &err)) {
            if (err.domain == LiteCoreDomain && err.code == kC4ErrorConflict)
                return rq.respondWithError(HTTPStatus::PreconditionFailed);
            else
                return rq.respondWithError(err);
        }
        rq.setStatus(HTTPStatus::Created, "Created");
    }


    void Listener::handleDeleteDatabase(RequestResponse &rq, C4Database *db) {
        if (!_allowDeleteDB)
            return rq.respondWithError(HTTPStatus::Forbidden, "Cannot delete databases");
        string name = rq.path(0);
        if (!unregisterDatabase(name))
            return rq.respondWithError(HTTPStatus::NotFound);
        C4Error err;
        if (!c4db_delete(db, &err)) {
            registerDatabase(name, db);
            return rq.respondWithError(err);
        }
    }


#pragma mark - DOCUMENT HANDLERS:


    void Listener::handleGetAllDocs(RequestResponse &rq, C4Database *db) {
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


    void Listener::handleGetDoc(RequestResponse &rq, C4Database *db) {
        string docID = rq.path(1);
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(db, slice(docID), true, &err);
        if (!doc)
            return rq.respondWithError(err);

        string revID = rq.query("rev");
        if (revID.empty()) {
            if (doc->flags & kDeleted)
                return rq.respondWithError(HTTPStatus::NotFound);
            revID = slice(doc->revID).asString();
        } else {
            if (!c4doc_selectRevision(doc, slice(revID), true, &err))
                return rq.respondWithError(err);
        }

        // Get the revision
        if (!doc->selectedRev.body.buf)
            return rq.respondWithError(HTTPStatus::NotFound);
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
    void Listener::handleModifyDoc(RequestResponse &rq, C4Database *db) {
        string docID = rq.path(1);                       // will be empty for POST

        // Parse the body:
        bool deleting = (rq.method() == "DELETE"_sl);
        Dict body = rq.bodyAsJSON().asDict();
        if (!body) {
            if (!deleting || rq.body())
                return rq.respondWithError(HTTPStatus::BadRequest);
        }

        // Get the revID from either the JSON body or the "rev" query param:
        slice revID = body["_rev"_sl].asString();
        string revIDQuery = rq.query("rev");
        if (!revIDQuery.empty()) {
            if (revID) {
                if (revID != slice(revIDQuery))
                    return rq.respondWithError(HTTPStatus::BadRequest);
            } else {
                revID = slice(revIDQuery);
            }
        }

        if (docID.empty()) {
            if (revID)
                return rq.respondWithError(HTTPStatus::BadRequest);            // Can't specify revID on a POST
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
            rq.setStatus(HTTPStatus::OK, "Deleted");
        else
            rq.setStatus(HTTPStatus::Created, "Created");
    }

} }
