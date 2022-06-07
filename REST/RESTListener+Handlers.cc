
// RESTListener+Handlers.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RESTListener.hh"
#include "c4Private.h"
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4DocEnumerator.hh"
#include "c4Document.hh"
#include "c4ReplicatorTypes.h"
#include "Server.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include "fleece/Expert.hh"
#include <functional>

using namespace std;
using namespace fleece;


namespace litecore { namespace REST {
    using namespace net;

#pragma mark - ROOT HANDLERS:


    void RESTListener::handleGetRoot(RequestResponse &rq) {
        alloc_slice version(c4_getVersion());
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("couchdb"_sl);
        json.writeString("Welcome"_sl);
        json.writeKey("vendor"_sl);
        json.beginDict();
        json.writeKey("name"_sl);
        json.writeString(kServerName);
        json.writeKey("version"_sl);
        json.writeString(version);
        json.endDict();
        json.writeKey("version"_sl);
        json.writeString(serverNameAndVersion());
        json.endDict();
    }


    void RESTListener::handleGetAllDBs(RequestResponse &rq) {
        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (string &name : databaseNames())
            json.writeString(name);
        json.endArray();
    }


    void RESTListener::handleActiveTasks(RequestResponse &rq) {
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

    
    void RESTListener::handleGetDatabase(RequestResponse &rq, C4Collection *coll) {
        auto docCount = coll->getDocumentCount();
        auto lastSequence = coll->getLastSequence();
        C4UUID uuid = coll->getDatabase()->getPublicUUID();
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
        json.writeUInt(uint64_t(lastSequence));
        json.writeKey("committed_update_seq"_sl);
        json.writeUInt(uint64_t(lastSequence));
        json.endDict();
    }


    void RESTListener::handleCreateDatabase(RequestResponse &rq) {
        if (!_allowCreateDB)
            return rq.respondWithStatus(HTTPStatus::Forbidden, "Cannot create databases");
        string dbName = rq.path(0);
        if (databaseNamed(dbName))
            return rq.respondWithStatus(HTTPStatus::PreconditionFailed, "Database exists");
        FilePath path;
        if (!pathFromDatabaseName(dbName, path))
            return rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid database name");

        auto db = C4Database::openNamed(dbName, {slice(path.dirName()), kC4DB_Create});
        _c4db_setDatabaseTag(db, DatabaseTag_RESTListener);
        registerDatabase(db, dbName);

        rq.respondWithStatus(HTTPStatus::Created, "Created");
    }


    void RESTListener::handleDeleteDatabase(RequestResponse &rq, C4Database *db) {
        if (!_allowDeleteDB)
            return rq.respondWithStatus(HTTPStatus::Forbidden, "Cannot delete databases");
        string name = rq.path(0);
        if (!unregisterDatabase(name))
            return rq.respondWithStatus(HTTPStatus::NotFound);
        try {
            db->closeAndDeleteFile();
        } catch (...) {
            registerDatabase(db, name);
            rq.respondWithError(C4Error::fromCurrentException());
        }
    }


#pragma mark - DOCUMENT HANDLERS:


    void RESTListener::handleGetAllDocs(RequestResponse &rq, C4Collection *coll) {
        // Apply options:
        C4EnumeratorOptions options;
        options.flags = kC4IncludeNonConflicted;
        if (rq.boolQuery("descending"))
            options.flags |= kC4Descending;
        bool includeDocs = rq.boolQuery("include_docs");
        if (includeDocs)
            options.flags |= kC4IncludeBodies;
        int64_t skip = rq.intQuery("skip", 0);
        int64_t limit = rq.intQuery("limit", INT64_MAX);
        // TODO: Implement startkey, endkey, etc.

        // Create enumerator:
        C4DocEnumerator e(coll, options);

        // Enumerate, building JSON:
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("rows"_sl);
        json.beginArray();
        while (e.next()) {
            if (skip-- > 0)
                continue;
            else if (limit-- <= 0)
                break;
            C4DocumentInfo info = e.documentInfo();
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
                json.writeKey("doc"_sl);
                expert(json).writeRaw(e.getDocument()->bodyAsJSON());
            }
            json.endDict();
        }
        json.endArray();
        json.endDict();
    }


    void RESTListener::handleGetDoc(RequestResponse &rq, C4Collection *coll) {
        string docID = rq.path(1);
        string revID = rq.query("rev");
        Retained<C4Document> doc = coll->getDocument(docID, true,
                                                  (revID.empty() ? kDocGetCurrentRev : kDocGetAll));
        if (doc) {
            if (revID.empty()) {
                if (doc->flags() & kDocDeleted)
                    doc = nullptr;
                else
                    revID = doc->revID().asString();
            } else {
                if (!doc->selectRevision(revID))
                    doc = nullptr;
            }
        }
        if (!doc)
            return rq.respondWithStatus(HTTPStatus::NotFound);

        // Get the revision
        alloc_slice json = doc->bodyAsJSON(false);

        // Splice the _id and _rev into the start of the JSON:
        rq.setHeader("Content-Type", "application/json");
        rq.write("{\"_id\":\"");
        rq.write(docID);
        rq.write("\",\"_rev\":\"");
        rq.write(revID);
        if (doc->selectedRev().flags & kRevDeleted)
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


    // Core code for create/update/delete operation on a single doc.
    bool RESTListener::modifyDoc(Dict body,
                                 string docID,
                                 string revIDQuery,
                                 bool deleting,
                                 bool newEdits,
                                 C4Collection *coll,
                                 fleece::JSONEncoder& json,
                                 C4Error *outError) noexcept
    {
        try {
            if (!deleting && !body) {
                c4error_return(WebSocketDomain, (int)HTTPStatus::BadRequest,
                               C4STR("body must be a JSON object"), outError);
                return false;
            }

            // Get the revID from either the JSON body or the "rev" query param:
            slice revID = body["_rev"_sl].asString();
            if (!revIDQuery.empty()) {
                if (!revID) {
                    revID = slice(revIDQuery);
                } else if (revID != slice(revIDQuery)) {
                    c4error_return(WebSocketDomain, (int)HTTPStatus::BadRequest,
                                   C4STR("\"_rev\" conflicts with ?rev"), outError);
                    return false;
                }
            }

            if (docID.empty()) {
                docID = slice(body["_id"].asString()).asString();
                if (docID.empty() && revID) {
                    // Can't specify revID on a POST
                    c4error_return(WebSocketDomain, (int)HTTPStatus::BadRequest,
                                   C4STR("Missing \"_id\""), outError);
                    return false;
                }
            }

            if (!newEdits && (!revID || docID.empty())) {
                c4error_return(WebSocketDomain, (int)HTTPStatus::BadRequest,
                    C4STR("Both \"_id\" and \"_rev\" must be given when \"new_edits\" is false"),
                    outError);
                return false;
            }

            if (body["_deleted"_sl].asBool())
                deleting = true;

            Retained<C4Document> doc;
            {
                C4Database::Transaction t(coll->getDatabase());

                // Encode body as Fleece (and strip _id and _rev):
                alloc_slice encodedBody;
                if (body)
                    encodedBody = doc->encodeStrippingOldMetaProperties(body, coll->getDatabase()->getFleeceSharedKeys());

                // Save the revision:
                C4Slice history[1] = {revID};
                C4DocPutRequest put = {};
                put.allocedBody = {(void*)encodedBody.buf, encodedBody.size};
                if (!docID.empty())
                    put.docID = slice(docID);
                put.revFlags = (deleting ? kRevDeleted : 0);
                put.existingRevision = !newEdits;
                put.allowConflict = false;
                put.history = history;
                put.historyCount = revID ? 1 : 0;
                put.save = true;

                doc = coll->putDocument(put, nullptr, outError);
                if (!doc)
                    return false;
                t.commit();
            }
            revID = slice(doc->selectedRev().revID);

            json.writeKey("ok"_sl);
            json.writeBool(true);
            json.writeKey("id"_sl);
            json.writeString(doc->docID());
            json.writeKey("rev"_sl);
            json.writeString(doc->selectedRev().revID);
            return true;
        } catch (...) {
            *outError = C4Error::fromCurrentException();
            return false;
        }
    }


    // This handles PUT and DELETE of a document, as well as POST to a database.
    void RESTListener::handleModifyDoc(RequestResponse &rq, C4Collection *coll) {
        string docID = rq.path(1);                       // will be empty for POST

        // Parse the body:
        bool deleting = (rq.method() == Method::DELETE);
        Dict body = rq.bodyAsJSON().asDict();
        if (!body) {
            if (!deleting || rq.body())
                return rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid JSON in request body");
        }

        auto &json = rq.jsonEncoder();
        json.beginDict();
        C4Error error;
        if (!modifyDoc(body, docID, rq.query("rev"), deleting, true, coll, json, &error)) {
            rq.respondWithError(error);
            return;
        }
        json.endDict();
        if (deleting)
            rq.setStatus(HTTPStatus::OK, "Deleted");
        else
            rq.setStatus(HTTPStatus::Created, "Created");
    }


    void RESTListener::handleBulkDocs(RequestResponse &rq, C4Collection *coll) {
        Dict body = rq.bodyAsJSON().asDict();
        Array docs = body["docs"].asArray();
        if (!docs)
            return rq.respondWithStatus(HTTPStatus::BadRequest,
                                        "Request body is invalid JSON, or has no \"docs\" array");

        Value v = body["new_edits"];
        bool newEdits = v ? v.asBool() : true;

        C4Database::Transaction t(coll->getDatabase());

        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (Array::iterator i(docs); i; ++i) {
            json.beginDict();
            Dict doc = i.value().asDict();
            C4Error error;
            if (!modifyDoc(doc, "", "", false, newEdits, coll, json, &error))
                rq.writeErrorJSON(error);
            json.endDict();
        }
        json.endArray();

        t.commit();
    }

} }
