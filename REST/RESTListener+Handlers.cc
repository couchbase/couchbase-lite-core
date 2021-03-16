
// RESTListener+Handlers.cc
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

#include "RESTListener.hh"
#include "c4.hh"
#include "c4Transaction.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4Document+Fleece.h"
#include "c4ReplicatorTypes.h"
#include "Server.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
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

    
    void RESTListener::handleGetDatabase(RequestResponse &rq, C4Database *db) {
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


    void RESTListener::handleCreateDatabase(RequestResponse &rq) {
        if (!_allowCreateDB)
            return rq.respondWithStatus(HTTPStatus::Forbidden, "Cannot create databases");
        string dbName = rq.path(0);
        if (auto db = databaseNamed(dbName); db)
            return rq.respondWithStatus(HTTPStatus::PreconditionFailed, "Database exists");
        FilePath path;
        if (!pathFromDatabaseName(dbName, path))
            return rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid database name");

        C4Error err;
        if (!openDatabase(dbName, path, kC4DB_Create, &err)) {
            if (err.domain == LiteCoreDomain && err.code == kC4ErrorConflict)
                return rq.respondWithStatus(HTTPStatus::PreconditionFailed);
            else
                return rq.respondWithError(err);
        }
        rq.respondWithStatus(HTTPStatus::Created, "Created");
    }


    void RESTListener::handleDeleteDatabase(RequestResponse &rq, C4Database *db) {
        if (!_allowDeleteDB)
            return rq.respondWithStatus(HTTPStatus::Forbidden, "Cannot delete databases");
        string name = rq.path(0);
        if (!unregisterDatabase(name))
            return rq.respondWithStatus(HTTPStatus::NotFound);
        C4Error err;
        if (!c4db_delete(db, &err)) {
            registerDatabase(db, name);
            return rq.respondWithError(err);
        }
    }


#pragma mark - DOCUMENT HANDLERS:


    void RESTListener::handleGetAllDocs(RequestResponse &rq, C4Database *db) {
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
        C4Error err;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, &options, &err);
        if (!e)
            return rq.respondWithError(err);

        // Enumerate, building JSON:
        auto &json = rq.jsonEncoder();
        json.beginDict();
        json.writeKey("rows"_sl);
        json.beginArray();
        while (c4enum_next(e, &err)) {
            if (skip-- > 0)
                continue;
            else if (limit-- <= 0)
                break;
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
                alloc_slice docBody = c4doc_bodyAsJSON(doc, false, &err);
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


    void RESTListener::handleGetDoc(RequestResponse &rq, C4Database *db) {
        string docID = rq.path(1);
        string revID = rq.query("rev");
        C4Error err;
        c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID), true,
                                             (revID.empty() ? kDocGetCurrentRev : kDocGetAll),
                                             &err);
        if (!doc)
            return rq.respondWithError(err);

        if (revID.empty()) {
            if (doc->flags & kDocDeleted)
                return rq.respondWithStatus(HTTPStatus::NotFound);
            revID = slice(doc->revID).asString();
        } else {
            if (!c4doc_selectRevision(doc, slice(revID), true, &err))
                return rq.respondWithError(err);
        }

        // Get the revision
        alloc_slice json = c4doc_bodyAsJSON(doc, false, &err);
        if (!json)
            return rq.respondWithError(err);

        // Splice the _id and _rev into the start of the JSON:
        rq.setHeader("Content-Type", "application/json");
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


    // Core code for create/update/delete operation on a single doc.
    bool RESTListener::modifyDoc(Dict body,
                             string docID,
                             string revIDQuery,
                             bool deleting,
                             bool newEdits,
                             C4Database *db,
                             fleece::JSONEncoder& json,
                             C4Error *outError)
    {
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
                           C4STR("Both \"_id\" and \"_rev\" must be given when \"new_edits\" is false"), outError);
            return false;
        }

        if (body["_deleted"_sl].asBool())
            deleting = true;

        c4::ref<C4Document> doc;
        {
            c4::Transaction t(db);
            if (!t.begin(outError))
                return false;

            // Encode body as Fleece (and strip _id and _rev):
            alloc_slice encodedBody;
            if (body) {
                encodedBody = c4doc_encodeStrippingOldMetaProperties(body,
                                                                     c4db_getFLSharedKeys(db),
                                                                     outError);
                if (!encodedBody)
                    return false;
            }

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
            doc = c4doc_put(db, &put, nullptr, outError);

            if (!doc || !t.commit(outError))
                return false;
        }
        revID = slice(doc->selectedRev.revID);

        json.writeKey("ok"_sl);
        json.writeBool(true);
        json.writeKey("id"_sl);
        json.writeString(doc->docID);
        json.writeKey("rev"_sl);
        json.writeString(doc->selectedRev.revID);
        return true;
    }


    // This handles PUT and DELETE of a document, as well as POST to a database.
    void RESTListener::handleModifyDoc(RequestResponse &rq, C4Database *db) {
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
        if (!modifyDoc(body, docID, rq.query("rev"), deleting, true, db, json, &error)) {
            rq.respondWithError(error);
            return;
        }
        json.endDict();
        if (deleting)
            rq.setStatus(HTTPStatus::OK, "Deleted");
        else
            rq.setStatus(HTTPStatus::Created, "Created");
    }


    void RESTListener::handleBulkDocs(RequestResponse &rq, C4Database *db) {
        Dict body = rq.bodyAsJSON().asDict();
        Array docs = body["docs"].asArray();
        if (!docs) {
            return rq.respondWithStatus(HTTPStatus::BadRequest, "Request body is invalid JSON, or has no \"docs\" array");
        }

        Value v = body["new_edits"];
        bool newEdits = v ? v.asBool() : true;

        C4Error error;
        c4::Transaction t(db);
        if (!t.begin(&error))
            return rq.respondWithStatus(HTTPStatus::BadRequest);

        auto &json = rq.jsonEncoder();
        json.beginArray();
        for (Array::iterator i(docs); i; ++i) {
            json.beginDict();
            Dict doc = i.value().asDict();
            if (!modifyDoc(doc, "", "", false, newEdits, db, json, &error))
                rq.writeErrorJSON(error);
            json.endDict();
        }
        json.endArray();

        if (!t.commit(&error))
            return rq.respondWithStatus(HTTPStatus::BadRequest);
    }

} }
