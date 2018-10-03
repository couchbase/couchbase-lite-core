//
// RESTListener+Replicate.cc
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
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4ListenerInternal.hh"
#include "Server.hh"
#include "Request.hh"
#include "RefCounted.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <time.h>

using namespace std;
using namespace fleece;


namespace litecore { namespace REST {

    class ReplicationTask : public RESTListener::Task {
    public:
        using Mutex = recursive_mutex;
        using Lock = unique_lock<Mutex>;

        ReplicationTask(RESTListener* listener, slice source, slice target, bool bidi, bool continuous)
        :Task(listener)
        ,_source(source)
        ,_target(target)
        ,_bidi(bidi)
        ,_continuous(continuous)
        { }

        bool start(C4Database *localDB, C4String localDbName,
                   const C4Address &remoteAddress, C4String remoteDbName,
                   C4ReplicatorMode pushMode, C4ReplicatorMode pullMode, C4Error *outError)
        {
            if (findMatchingTask()) {
                c4error_return(WebSocketDomain, 409, C4STR("Equivalent replication already running"),
                               outError);
                return false;
            }

            Lock lock(_mutex);
            _push = (pushMode >= kC4OneShot);
            registerTask();
            c4log(RESTLog, kC4LogInfo,
                  "Replicator task #%d starting: local=%.*s, mode=%s, scheme=%.*s, host=%.*s,"
                  " port=%u, db=%.*s, bidi=%d, continuous=%d",
                  taskID(), SPLAT(localDbName), (pushMode > kC4Disabled ? "push" : "pull"),
                  SPLAT(remoteAddress.scheme), SPLAT(remoteAddress.hostname), remoteAddress.port,
                  SPLAT(remoteDbName),
                  _bidi, _continuous);
            
            C4ReplicatorParameters params = {};
            params.push = pushMode;
            params.pull = pullMode;
            params.onStatusChanged = [](C4Replicator*, C4ReplicatorStatus status, void *context) {
                ((ReplicationTask*)context)->onReplStateChanged(status);
            };
            params.callbackContext = this;
            _repl = c4repl_new(localDB, remoteAddress, remoteDbName, nullptr, params, outError);
            if (!_repl) {
                c4log(RESTLog, kC4LogInfo,
                      "Replicator task #%d failed to start!", taskID());
                unregisterTask();
                return false;
            }
            onReplStateChanged(c4repl_getStatus(_repl));
            return true;
        }

        ReplicationTask* findMatchingTask() {
            for (auto task : listener()->tasks()) {
                // Note that either direction is considered a match
                ReplicationTask *repl = dynamic_cast<ReplicationTask*>(task.get());
                if (repl && ((repl->_source == _source && repl->_target == _target) ||
                             (repl->_source == _target && repl->_target == _source))) {
                    return repl;
                }
            }
            return nullptr;
        }

        // Cancel any existing task with the same parameters as me:
        bool cancelExisting() {
            auto task = findMatchingTask();
            if (task) {
                task->stop();
                return true;
            }
            return false;
        }

        virtual bool finished() const override {
            Lock lock(const_cast<ReplicationTask*>(this)->_mutex);
            return _finalResult != HTTPStatus::undefined;
        }

        C4ReplicatorStatus status() {
            Lock lock(_mutex);
            return _status;
        }

        alloc_slice message() {
            Lock lock(_mutex);
            return _message;
        }

        virtual void writeDescription(fleece::JSONEncoder &json) override {
            Task::writeDescription(json);

            json.writeKey("type"_sl);
            json.writeString("replication"_sl);
            json.writeKey("session_id"_sl);
            json.writeUInt(taskID());
            json.writeKey("source"_sl);
            json.writeString(_source);
            json.writeKey("target"_sl);
            json.writeString(_target);
            if (_continuous) {
                json.writeKey("continuous"_sl);
                json.writeBool(true);
            }
            if (_bidi) {
                json.writeKey("bidi"_sl);
                json.writeBool(true);
            }

            Lock lock(_mutex);

            json.writeKey("updated_on"_sl);
            json.writeUInt(_timeUpdated);

            static slice const kStatusName[] = {"Stopped"_sl, "Offline"_sl, "Connecting"_sl,
                "Idle"_sl, "Active"_sl};
            json.writeKey("status"_sl);
            json.writeString(kStatusName[_status.level]);

            if (_status.error.code) {
                json.writeKey("error"_sl);
                writeErrorInfo(json);
            }

            if (_status.progress.unitsTotal > 0) {
                double fraction = _status.progress.unitsCompleted * 100.0 / _status.progress.unitsTotal;
                json.writeKey("progress"_sl);
                json.writeInt(int64_t(fraction));
            }

            if (_status.progress.documentCount > 0) {
                slice key;
                if (_bidi)
                    key = "docs_transferred"_sl;
                else
                    key = _push ? "docs_written"_sl : "docs_read"_sl;
                json.writeKey(key);
                json.writeUInt(_status.progress.documentCount);
            }
        }


        void writeErrorInfo(JSONEncoder &json) {
            Lock lock(_mutex);
            json.beginDict();
            json.writeKey("error"_sl);
            json.writeString(_message);
            json.writeKey("x-litecore-domain"_sl);
            json.writeInt(_status.error.domain);
            json.writeKey("x-litecore-code"_sl);
            json.writeInt(_status.error.code);
            json.endDict();
        }


        HTTPStatus wait() {
            Lock lock(_mutex);
            _cv.wait(lock, [this]{return finished();});
            return _finalResult;
        }


        void stop() override {
            Lock lock(_mutex);
            if (_repl) {
                c4log(RESTLog, kC4LogInfo, "Replicator task #%u stopping...", taskID());
                c4repl_stop(_repl);
            }
        }

    private:
        void onReplStateChanged(const C4ReplicatorStatus &status) {
            {
                Lock lock(_mutex);
                _status = status;
                _message = c4error_getMessage(status.error);
                if (status.level == kC4Stopped) {
                    _finalResult = status.error.code ? HTTPStatus::GatewayError
                                                     : HTTPStatus::OK;
                    _repl = nullptr;
                }
                time(&_timeUpdated);
            }
            if (finished()) {
                c4log(RESTLog, kC4LogInfo, "Replicator task #%u finished", taskID());
                _cv.notify_all();
            }
            //unregisterTask();  --no, leave it so a later call to _active_tasks can get its state
        }

        alloc_slice _source, _target;
        bool _bidi, _continuous, _push;
        Mutex _mutex;
        condition_variable_any _cv;
        c4::ref<C4Replicator> _repl;
        C4ReplicatorStatus _status;
        alloc_slice _message;
        HTTPStatus _finalResult {HTTPStatus::undefined};
    };


#pragma mark - HTTP HANDLER:


    void RESTListener::handleReplicate(litecore::REST::RequestResponse &rq) {
        // Parse the JSON body:
        auto params = rq.bodyAsJSON().asDict();
        slice source = params["source"].asString();
        slice target = params["target"].asString();
        if (!source || !target)
            return rq.respondWithStatus(HTTPStatus::BadRequest, "Missing source or target parameters");

        bool bidi = params["bidi"].asBool();
        bool continuous = params["continuous"].asBool();
        C4ReplicatorMode activeMode = continuous ? kC4Continuous : kC4OneShot;

        slice localName;
        slice remoteURL;
        C4ReplicatorMode pushMode, pullMode;
        pushMode = pullMode = (bidi ? activeMode : kC4Disabled);
        if (c4repl_isValidDatabaseName(source)) {
            localName = source;
            pushMode = activeMode;
            remoteURL = target;
        } else if (c4repl_isValidDatabaseName(target)) {
            localName = target;
            pullMode = activeMode;
            remoteURL = source;
        } else {
            return rq.respondWithStatus(HTTPStatus::BadRequest,
                                       "Neither source nor target is a local database name");
        }

        c4::ref<C4Database> localDB = databaseNamed(localName.asString());
        if (!localDB)
            return rq.respondWithStatus(HTTPStatus::NotFound);

        C4Address remoteAddress;
        C4String remoteDbName;
        if (!c4address_fromURL(remoteURL, &remoteAddress, &remoteDbName))
            return rq.respondWithStatus(HTTPStatus::BadRequest, "Invalid database URL");

        // Start the replication!
        C4Error error;
        Retained<ReplicationTask> task = new ReplicationTask(this, source, target, bidi, continuous);

        if (params["cancel"].asBool()) {
            // Hang on, stop the presses -- we're canceling, not starting
            bool canceled = task->cancelExisting();
            rq.setStatus(canceled ? HTTPStatus::OK : HTTPStatus::NotFound,
                         canceled ? "Stopped" : "No matching task");
            return;
        }

        if (!task->start(localDB, localName,
                         remoteAddress, remoteDbName,
                         pushMode, pullMode, &error))
            return rq.respondWithError(error);


        HTTPStatus statusCode = HTTPStatus::OK;
        if (!continuous) {
            statusCode = task->wait();
            task->unregisterTask();
        }

        auto &json = rq.jsonEncoder();
        if (statusCode == HTTPStatus::OK) {
            json.beginDict();
            json.writeKey("ok"_sl);
            json.writeBool(true);
            json.writeKey("session_id"_sl);
            json.writeUInt(task->taskID());
            json.endDict();
        } else {
            task->writeErrorInfo(json);
        }
        rq.setStatus(statusCode, task->message().asString().c_str());
    }

} }
