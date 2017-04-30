//
//  Listener+Replicate.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Listener.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "Server.hh"
#include "Request.hh"
#include "Logging.hh"
#include "RefCounted.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <functional>
#include <time.h>

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace REST {

    class ReplicationTask : public Listener::Task {
    public:
        ReplicationTask(Listener* listener, slice source, slice target, bool continuous)
        :Task(listener)
        ,_source(source)
        ,_target(target)
        ,_continuous(continuous)
        { }

        bool start(C4Database *localDB, const C4Address &remoteAddress, C4String remoteDbName,
                   C4ReplicatorMode pushMode, C4ReplicatorMode pullMode, C4Error *outError)
        {
            auto callback = [](C4Replicator*, C4ReplicatorStatus status, void *context) {
                ((ReplicationTask*)context)->onReplStateChanged(status);
            };
            _repl = c4repl_new(localDB, remoteAddress, remoteDbName, nullptr,
                               pushMode, pullMode,
                               callback, this,
                               outError);
            if (!_repl)
                return false;
            _push = (pushMode >= kC4OneShot);
            onReplStateChanged(c4repl_getStatus(_repl));
            registerTask();
            return true;
        }

        virtual bool finished() const override {
            return _finalResult != HTTPStatus::undefined;
        }

        C4ReplicatorStatus status() {
            lock_guard<mutex> lock(_mutex);
            return _status;
        }

        alloc_slice message() {
            lock_guard<mutex> lock(_mutex);
            return _message;
        }

        virtual void writeDescription(fleeceapi::JSONEncoder &json) override {
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

            unique_lock<mutex> lock(_mutex);

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

            if (_status.progress.total) {
                double fraction = _status.progress.completed * 100.0 / _status.progress.total;
                json.writeKey("progress"_sl);
                json.writeInt(int64_t(fraction));
            }

            if (_status.progress.completed > 0) {
                json.writeKey(_push ? "docs_written"_sl : "docs_read"_sl);
                json.writeUInt(_status.progress.completed);
            }
        }


        void writeErrorInfo(JSONEncoder &json) {
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
            unique_lock<mutex> lock(_mutex);
            _cv.wait(lock, [this]{return finished();});
            return _finalResult;
        }

    private:
        void onReplStateChanged(const C4ReplicatorStatus &status) {
            {
                lock_guard<mutex> lock(_mutex);
                _status = status;
                _message = c4error_getMessage(status.error);
                if (status.level == kC4Stopped) {
                    _finalResult = status.error.code ? HTTPStatus::GatewayError
                                                     : HTTPStatus::OK;
                    _repl = nullptr;
                    Log("Replicator finished");
                }
                time(&_timeUpdated);
            }
            if (finished())
                _cv.notify_all();
            //unregisterTask();  --no, leave it so a later call to _active_tasks can get its state
        }

        Listener* _listener;
        alloc_slice _source, _target;
        bool _continuous, _push;
        mutex _mutex;
        condition_variable _cv;
        c4::ref<C4Replicator> _repl;
        C4ReplicatorStatus _status;
        alloc_slice _message;
        HTTPStatus _finalResult {HTTPStatus::undefined};
    };


#pragma mark - HTTP HANDLER:


    void Listener::handleReplicate(litecore::REST::RequestResponse &rq) {
        // Parse the JSON body:
        auto params = rq.bodyAsJSON().asDict();
        slice source = params["source"].asString();
        slice target = params["target"].asString();
        if (!source || !target)
            return rq.respondWithError(HTTPStatus::BadRequest, "Missing source or target parameters");

        slice localName;
        slice remoteURL;
        C4ReplicatorMode pushMode = kC4Disabled, pullMode = kC4Disabled;
        if (c4repl_isValidDatabaseName(source)) {
            localName = source;
            pushMode = kC4OneShot;
            remoteURL = target;
        } else if (c4repl_isValidDatabaseName(target)) {
            localName = target;
            pullMode = kC4OneShot;
            remoteURL = source;
        } else {
            return rq.respondWithError(HTTPStatus::BadRequest,
                                       "Neither source nor target is a local database name");
        }

        c4::ref<C4Database> localDB = databaseNamed(localName.asString());
        if (!localDB)
            return rq.respondWithError(HTTPStatus::NotFound);

        C4Address remoteAddress;
        C4String remoteDbName;
        if (!c4repl_parseURL(remoteURL, &remoteAddress, &remoteDbName))
            return rq.respondWithError(HTTPStatus::BadRequest, "Invalid database URL");
        bool continuous = params["continuous"].asBool();

        Log("Replicating: local=%.*s, mode=%s, scheme=%.*s, host=%.*s, port=%u, db=%.*s",
            SPLAT(localName),
            (pushMode > kC4Disabled ? "push" : "pull"),
            SPLAT(remoteAddress.scheme), SPLAT(remoteAddress.hostname), remoteAddress.port,
            SPLAT(remoteDbName));

        // Start the replication!
        C4Error error;
        Retained<ReplicationTask> task = new ReplicationTask(this, source, target, continuous);
        if (!task->start(localDB, remoteAddress, remoteDbName, pushMode, pullMode, &error))
            return rq.respondWithError(error);


        HTTPStatus statusCode = HTTPStatus::OK;
        if (!continuous) {
            Log("Waiting for replicator to complete...");
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
