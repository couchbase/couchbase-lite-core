//
// RESTListener+Replicate.cc
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
#include "c4Database.hh"
#include "c4Replicator.hh"
#include "c4ListenerInternal.hh"
#include "fleece/RefCounted.hh"
#include "ReplicatorOptions.hh"
#include "StringUtil.hh"
#include "fleece/Mutable.hh"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <ctime>

using namespace std;
using namespace fleece;

namespace litecore::REST {
    using namespace net;

    class RESTListener::ReplicationTask : public RESTListener::Task {
      public:
        ReplicationTask(RESTListener* listener, slice source, slice target)
            : Task(listener), _source(source), _target(target) {}

        bool start(C4Database* localDB, C4String localDbName, const C4Address& remoteAddress, C4String remoteDbName,
                   C4ReplicatorParameters& params) {
            unique_lock lock(_mutex);
            registerTask();

            auto& coll  = params.collections[0];
            _push       = coll.push >= kC4OneShot;
            _bidi       = (_push && coll.push >= kC4OneShot);
            _continuous = (coll.pull == kC4Continuous || coll.push == kC4Continuous);

            params.callbackContext = this;
            params.onStatusChanged = [](C4Replicator*, C4ReplicatorStatus status, void* context) {
                ((ReplicationTask*)context)->onReplStateChanged(status);
            };

            c4log(ListenerLog, kC4LogInfo,
                  "Replicator task #%d starting: local=%.*s, mode=%s, scheme=%.*s, host=%.*s,"
                  " port=%u, db=%.*s, bidi=%d, continuous=%d",
                  taskID(), SPLAT(localDbName), (_push ? "push" : "pull"), SPLAT(remoteAddress.scheme),
                  SPLAT(remoteAddress.hostname), remoteAddress.port, SPLAT(remoteDbName), _bidi, _continuous);

            try {
                C4Database::WithClientMutex with(localDB);
                _repl = localDB->newReplicator(remoteAddress, remoteDbName, params);
                _repl->start();
                if ( _repl ) {  // it is possible that the replicator already stopped and I cleared the ref
                    onReplStateChanged(_repl->getStatus());
                }
            } catch ( ... ) {
                c4log(ListenerLog, kC4LogInfo, "Replicator task #%d failed to start!", taskID());
                unregisterTask();
                _status = {
                        .level = kC4Stopped,
                        .error = C4Error::fromCurrentException(),
                };
                _message  = _status.error.message();
                _finished = true;
            }
            return _status.error == kC4NoError;
        }

        bool finished() const override {
            unique_lock lock(_mutex);
            return _finished;
        }

        C4ReplicatorStatus status() const {
            unique_lock lock(_mutex);
            return _status;
        }

        alloc_slice message() const {
            unique_lock lock(_mutex);
            return _message;
        }

        void writeDescription(fleece::JSONEncoder& json) override {
            Task::writeDescription(json);
            json.writeFormatted(
                    "type:'replication', session_id:%u, source:%.*s, target:%.*s, continuous: %-c, bidi: %-c", taskID(),
                    SPLAT(_source), SPLAT(_target), char(_continuous), char(_bidi));

            unique_lock lock(_mutex);

            json.writeKey("updated_on"_sl);
            json.writeUInt(timeUpdated());

            static slice const kStatusName[] = {"Stopped"_sl, "Offline"_sl, "Connecting"_sl, "Idle"_sl, "Active"_sl};
            json.writeKey("status"_sl);
            json.writeString(kStatusName[_status.level]);

            if ( _status.error.code ) {
                json.writeKey("error"_sl);
                writeErrorInfo(json);
            }

            if ( _status.progress.unitsTotal > 0 ) {
                double percent = narrow_cast<double>(_status.progress.unitsCompleted) * 100.0
                                 / narrow_cast<double>(_status.progress.unitsTotal);
                json["progress"] = int64_t(percent);
            }

            if ( _status.progress.documentCount > 0 ) {
                slice key;
                if ( _bidi ) key = "docs_transferred"_sl;
                else
                    key = _push ? "docs_written"_sl : "docs_read"_sl;
                json.writeKey(key);
                json.writeUInt(_status.progress.documentCount);
            }
        }

        void writeErrorInfo(JSONEncoder& json) const {
            unique_lock lock(_mutex);
            json.writeFormatted("{error: %.*s, 'x-litecore-domain': %d, 'x-litecore-code': %d}", SPLAT(_message),
                                _status.error.domain, _status.error.code);
        }

        void stop() override {
            unique_lock lock(_mutex);
            if ( _repl ) {
                c4log(ListenerLog, kC4LogInfo, "Replicator task #%u stopping...", taskID());
                _repl->stop();
            }
        }

      private:
        void onReplStateChanged(const C4ReplicatorStatus& status) {
            {
                unique_lock lock(_mutex);
                _status  = status;
                _message = c4error_getMessage(status.error);
                if ( status.level == kC4Stopped ) {
                    _finished = true;
                    _repl     = nullptr;
                }
                bumpTimeUpdated();
            }
            if ( finished() ) {
                c4log(ListenerLog, kC4LogInfo, "Replicator task #%u finished", taskID());
                //unregisterTask();  --no, leave it so a later call to _active_tasks can get its state
            }
        }

        alloc_slice            _source, _target;
        bool                   _bidi = false, _continuous = false, _push = false;
        Retained<C4Replicator> _repl;
        C4ReplicatorStatus     _status{};
        alloc_slice            _message;
        bool                   _finished = false;
    };

#pragma mark - HTTP HANDLER:

    void RESTListener::handleReplicate(litecore::REST::RequestResponse& rq) {
        auto params = rq.bodyAsJSON().asDict();
        if ( !params ) {
            return rq.respondWithStatus(HTTPStatus::BadRequest,
                                        "Invalid JSON in request body (or body is not an object)");
        } else if ( Value cancelVal = params["cancel"] ) {
            // Hang on, stop the presses -- we're canceling, not starting
            cancelReplication(rq, cancelVal);
        } else {
            auto [status, message, task] = _handleReplicate(params);
            if ( task ) {
                if ( task->status().error == kC4NoError )
                    rq.jsonEncoder().writeFormatted("{ok: true, session_id: %u}", task->taskID());
                else
                    task->writeErrorInfo(rq.jsonEncoder());
                rq.setStatus(status, message.c_str());
            } else {
                rq.respondWithStatus(status, message.c_str());
            }
        }
    }

    unsigned RESTListener::startReplication(Dict params) {
        auto [status, message, task] = _handleReplicate(params);
        if ( int(status) < 300 ) {
            return task->taskID();
        } else {
            error(error::WebSocket, int(status), message)._throw();
        }
    }

    static string validateKeys(const char* what, Dict dict, unordered_set<string_view> const& keys) {
        if ( !dict ) return stringprintf("%s must be a JSON object", what);
        for ( Dict::iterator i(dict); i; ++i ) {
            if ( slice key = i.keyString(); !keys.contains(key) ) [[unlikely]]
                return stringprintf("Unknown key '%.*s' in %s", FMTSLICE(key), what);
        }
        return "";
    }

    tuple<HTTPStatus, string, RESTListener::ReplicationTask*> RESTListener::_handleReplicate(Dict params) {
        static const unordered_set<string_view> allowedKeys{"source",   "target",  "bidi", "continuous", "collections",
                                                            "channels", "doc_ids", "user", "password"};
        static const unordered_set<string_view> allowedCollKeys{"channels", "doc_ids"};
        if ( string msg = validateKeys("replication parameters", params, allowedKeys); !msg.empty() )
            return {HTTPStatus::BadRequest, std::move(msg), nullptr};

        bool             bidi       = params["bidi"].asBool();
        bool             continuous = params["continuous"].asBool();
        C4ReplicatorMode activeMode = continuous ? kC4Continuous : kC4OneShot;

        // Get the local DB and remote URL:
        slice source = params["source"].asString();
        slice target = params["target"].asString();
        if ( !source || !target ) return {HTTPStatus::BadRequest, "Missing source or target parameters", nullptr};
        slice            localName;
        slice            remoteURL;
        C4ReplicatorMode pushMode, pullMode;
        pushMode = pullMode = (bidi ? activeMode : kC4Disabled);
        if ( C4Replicator::isValidDatabaseName(source) ) {
            localName = source;
            pushMode  = activeMode;
            remoteURL = target;
        } else if ( C4Replicator::isValidDatabaseName(target) ) {
            localName = target;
            pullMode  = activeMode;
            remoteURL = source;
        } else {
            return {HTTPStatus::BadRequest, "Neither source nor target is a local database name", nullptr};
        }
        Retained<C4Database> localDB = databaseNamed(localName.asString());
        if ( !localDB ) return {HTTPStatus::NotFound, "", nullptr};
        C4Address remoteAddress;
        slice     remoteDbName;
        if ( !C4Address::fromURL(remoteURL, &remoteAddress, &remoteDbName) )
            return {HTTPStatus::BadRequest, "Invalid database URL", nullptr};

        // Subroutine that adds a C4ReplicationCollection:
        vector<alloc_slice>                  collOptions;  // backing store for each optionsDictFleece
        std::vector<C4ReplicationCollection> replCollections;
        auto                                 addCollection = [&](slice collPath, Dict collParams) {
            C4CollectionSpec collSpec = repl::Options::collectionPathToSpec(collPath);
            Encoder          enc;
            enc.beginDict();
            if ( Array channels = collParams["channels"].asArray() ) enc[kC4ReplicatorOptionChannels] = channels;
            if ( Array docIDs = collParams["doc_ids"].asArray() ) enc[kC4ReplicatorOptionDocIDs] = docIDs;
            enc.endDict();
            alloc_slice options = enc.finish();
            collOptions.push_back(options);
            replCollections.push_back({
                                                    .collection        = collSpec,
                                                    .push              = pushMode,
                                                    .pull              = pullMode,
                                                    .optionsDictFleece = options,
            });
        };

        // Get the collection(s):
        if ( auto collectionsVal = params["collections"] ) {
            if ( Array collectionNames = collectionsVal.asArray() ) {
                // `collections` is an array of collection names:
                for ( Array::iterator iter(collectionNames); iter; iter.next() ) {
                    slice collPath = iter.value().asString();
                    if ( !collPath ) return {HTTPStatus::BadRequest, "Invalid collections item", nullptr};
                    addCollection(collPath, params);
                }
            } else if ( Dict collections = collectionsVal.asDict() ) {
                // 'collections' is a dictionary mapping collection names to options:
                for ( Dict::iterator iter(collections); iter; iter.next() ) {
                    Dict collParams = iter.value().asDict();
                    if ( string msg = validateKeys("collection parameters", collParams, allowedCollKeys); !msg.empty() )
                        return {HTTPStatus::BadRequest, std::move(msg), nullptr};
                    addCollection(iter.keyString(), collParams);
                }
            } else {
                return {HTTPStatus::BadRequest, "'collections' must be an array or object", nullptr};
            }
        } else {
            // 'collections' is missing; just use the default collection:
            addCollection(kC4DefaultCollectionName, params);
        }
        if ( replCollections.empty() )
            return {HTTPStatus::BadRequest, "At least one collection must be replicated", nullptr};
        for ( size_t i = 0; i < replCollections.size(); i++ ) {
            for ( size_t j = 0; j < i; j++ ) {
                if ( replCollections[j].collection == replCollections[i].collection )
                    return {HTTPStatus::BadRequest, "Duplicate collection", nullptr};
            }
        }

        // Encode the outer Fleece-based C4Replicator options:
        Encoder enc;
        enc.beginDict();
        if ( slice user = params["user"].asString() ) {
            slice password = params["password"].asString();
            enc.writeKey(kC4ReplicatorOptionAuthentication);
            enc.beginDict();
            enc[kC4ReplicatorAuthType]     = kC4AuthTypeBasic;
            enc[kC4ReplicatorAuthUserName] = user;
            enc[kC4ReplicatorAuthPassword] = password;
            enc.endDict();
        }
        enc.endDict();
        alloc_slice options = enc.finish();

        // Start the replicator!
        Retained<ReplicationTask> task = new ReplicationTask(this, source, target);
        C4ReplicatorParameters    c4Params{
                   .optionsDictFleece = options,
                   .collections       = replCollections.data(),
                   .collectionCount   = replCollections.size(),
        };
        bool ok = task->start(localDB, localName, remoteAddress, remoteDbName, c4Params);
        if ( ok ) {
            return {HTTPStatus::Accepted, "Replication has started", task};
        } else {
            string     message    = task->message().asString();
            HTTPStatus statusCode = RequestResponse::errorToStatus(task->status().error);
            return {statusCode, message, task};
        }
    }

    void RESTListener::cancelReplication(litecore::REST::RequestResponse& rq, Value taskIDVal) {
        auto cancelID = taskIDVal.asInt();
        if ( cancelID <= 0 )
            return rq.respondWithStatus(HTTPStatus::BadRequest, "'cancel' must be a positive integer session_id");
        auto        status  = HTTPStatus::NotFound;
        const char* message = "No active task with that session_id";
        for ( auto& task : tasks() ) {
            if ( task->taskID() == cancelID && !task->finished() ) {
                if ( dynamic_cast<ReplicationTask*>(task.get()) ) {
                    task->stop();
                    status  = HTTPStatus::OK;
                    message = "Stopped";
                } else {
                    status  = HTTPStatus::Forbidden;
                    message = "Task is not a replicator";
                }
                break;
            }
        }
        rq.respondWithStatus(status, message);
    }

    void RESTListener::handleSync(RequestResponse& rq, C4Database*) {
        rq.setStatus(HTTPStatus::NotImplemented, nullptr);
    }


}  // namespace litecore::REST
