// RESTListener+Changes.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// Reference:
// https://docs.couchbase.com/sync-gateway/current/rest-api.html#/Database_Management/get_keyspace__changes
// https://docs.couchdb.org/en/stable/api/database/changes.html

#include "RESTListener.hh"
#include "DatabasePool.hh"
#include "Error.hh"
#include "Timer.hh"
#include "c4ListenerInternal.hh"
#include "c4Private.h"
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "c4Observer.hh"
#include "fleece/Expert.hh"
#include <functional>
#include <sys/socket.h>

using namespace std;
using namespace fleece;

namespace litecore::REST {
    using namespace net;

    static constexpr uint64_t kDefaultHeartbeatMS = 60000;
    static constexpr uint64_t kMinHeartbeatMS     = 5000;
    static constexpr uint64_t kMaxHeartbeatMS     = 60000;
    static constexpr uint64_t kDefaultTimeoutMS   = 60000;
    static constexpr uint64_t kMaxTimeoutMS       = 60000;

    static constexpr size_t kFlushAtSize = 32768;

    class ChangesTask : public RESTListener::Task {
      public:
        enum FeedType { normal, longpoll, continuous, unknown };

        explicit ChangesTask(RESTListener* listener, RequestResponse&& rq)
            : Task(listener)
            , _rq(std::move(rq))
            , _limit{_rq.uintQuery("limit", INT64_MAX)}
            , _enumFlags{kC4IncludeNonConflicted | kC4IncludeDeleted} {
            if ( string feed = _rq.query("feed"); feed.empty() || feed == "normal" ) _feed = normal;
            else if ( feed == "longpoll" )
                _feed = longpoll;
            else if ( feed == "continuous" )
                _feed = continuous;
            if ( _rq.boolQuery("include_docs") ) _enumFlags |= kC4IncludeBodies;
            if ( _rq.boolQuery("active_only") ) _enumFlags &= ~kC4IncludeDeleted;  // SG addition to API
            if ( _rq.boolQuery("descending") ) _enumFlags |= kC4Descending;
            //TODO: ?doc_ids, ?filter
        }

        ~ChangesTask() { c4log(ListenerLog, kC4LogVerbose, "ChangesTask %p deleted", this); }

        bool runImmediate(C4Collection* coll) {
            _rq.uncacheable();
            _rq.setHeader("Connection", "close");  // request is disconnected from Server
            if ( _feed == unknown ) {
                _rq.respondWithStatus(HTTPStatus::BadRequest, "unsupported feed type");
                _rq.finish();
                return true;
            } else if ( _feed == continuous ) {
                _rq.setHeader("Content-Type", "text/plain; charset=utf-8");  // JSON-per-line is not technically JSON
                _rq.setChunked();
            } else {
                _rq.setHeader("Content-Type", "application/json");
                _rq.write("{\"results\": [\n");
            }

            _lastSequence = coll->getLastSequence();
            C4SequenceNumber since =
                    _rq.query("since") == "now" ? _lastSequence : C4SequenceNumber(_rq.uintQuery("since"));
            if ( since < _lastSequence ) {
                C4DocEnumerator e(coll, since, {_enumFlags});
                while ( _limit > 0 && e.next() ) {
                    alloc_slice body;
                    if ( _enumFlags & kC4IncludeBodies ) body = e.getDocument()->bodyAsJSON();
                    writeChange(e.documentInfo(), body);
                }
            }

            if ( _feed == continuous || (_feed == longpoll && _sent == 0 && _limit > 0) ) {
                // In continuous mode, we always wait for more changes.
                // In longpoll mode, we wait if there are no changes yet.
                _rq.flush();
                wait(coll);
                return false;
            } else {
                _rq.printf("], \"last_seq\":%llu}", uint64_t(_lastSequence));
                _rq.finish();
                return true;
            }
        }

        RequestResponse&& extractRequest() && {return std::move(_rq);}

        void writeDescription(fleece::JSONEncoder& json) override {
            Task::writeDescription(json);
            unique_lock lock(_mutex);
            string_view args = _rq.queries();
            json.writeFormatted("type:'changes', args:%.*s", int(args.size()), args.data());
        }

        bool finished() const override {
            unique_lock lock(_mutex);
            return _rq.finished();
        }

        void stop() override {
            {
                unique_lock lock(_mutex);
                if ( !_rq.finished() ) {
                    if ( _feed != continuous && !_rq.socketError() )
                        _rq.printf("\n], \"last_seq\":%llu}", uint64_t(_lastSequence));
                    _rq.finish();
                }
                c4log(ListenerLog, kC4LogInfo, "End of socket connection from %s (%s)", _rq.peerAddress().c_str(),
                    (_stopMessage ? _stopMessage : ""));
            }
            unregisterTask();  // this will probably delete me
        }

        void stop(const char* message) {
            _stopMessage = message;
            stop();
        }

      private:
        void writeChange(C4DocumentInfo const& info, slice bodyJSON) {
            DebugAssert(_limit > 0);
            --_limit;
            if ( _sent > 0 && _feed != continuous ) _rq.write(",\n");
            ++_sent;
            JSONEncoder json;
            json.writeDict([&] {
                json.writeFormatted("seq:%llu, id:%.*s, deleted:%-c, changes:[{rev:%.*s}]", uint64_t(info.sequence),
                                    SPLAT(info.docID), char((info.flags & kDocDeleted) != 0), SPLAT(info.revID));
                if ( bodyJSON ) {
                    json.writeKey("doc");
                    json.writeRaw(bodyJSON);
                }
            });
            _rq.write(json.finish());
            if ( _feed == continuous ) _rq.write("\n");
            _rq.flush(kFlushAtSize);
        }

        // Go into asynchronous mode, waiting for changes or timeout, possibly sending heartbeats.
        void wait(C4Collection* coll) {
            _observer = C4CollectionObserver::create(coll, [this](auto) { this->observeChange(); });
            if ( string hb = _rq.query("heartbeat"); !hb.empty() ) {
                // Set a heartbeat timer that sends a newline periodically:
                uint64_t intervalMS = kDefaultHeartbeatMS;
                if ( hb != "true" ) intervalMS = _rq.uintQuery("heartbeat", kDefaultHeartbeatMS);
                if ( intervalMS > 0 ) {
                    intervalMS = std::max(intervalMS, kMinHeartbeatMS);
                    intervalMS = std::min(intervalMS, kMaxHeartbeatMS);
                    chrono::milliseconds interval(intervalMS);
                    _heartbeatTimer.emplace([this, interval] { this->heartbeat(interval); });
                    _heartbeatTimer->fireAfter(interval);
                }
            }
            if ( !_heartbeatTimer ) {
                // Or set a timeout timer to stop the task:
                auto timeout = _rq.uintQuery("timeout", kDefaultTimeoutMS);
                if ( timeout == 0 ) timeout = kMaxTimeoutMS;
                timeout = std::min(timeout, kMaxTimeoutMS);
                _timeoutTimer.emplace([this] { this->timeout(); });
                _timeoutTimer->fireAfter(chrono::milliseconds(timeout));
            }
            c4log(ListenerLog, kC4LogVerbose, "ChangesTask %p waiting...", this);
            registerTask();
        }

        // Called (on some unknown thread) when the collection has changed.
        void observeChange() {
            c4log(ListenerLog, kC4LogVerbose, "ChangesTask %p got changes!", this);
            {
                unique_lock lock(_mutex);
                if ( _rq.finished() ) return;
                bumpTimeUpdated();

                BorrowedDatabase           db;
                C4Collection*              coll = nullptr;
                C4DatabaseObserver::Change c4changes[100];
                auto                       prevLastSequence = _lastSequence;
                while ( _limit > 0 ) {
                    auto                    maxChanges = std::min(size_t(_limit), std::size(c4changes));
                    C4CollectionObservation o          = _observer->getChanges(c4changes, uint32_t(maxChanges));
                    if ( o.numChanges == 0 ) break;  // Received all changes
                    _lastSequence = c4changes[o.numChanges - 1].sequence;
                    if ( _enumFlags & kC4Descending ) { std::reverse(&c4changes[0], &c4changes[o.numChanges]); }
                    for ( auto c4change = &c4changes[0]; c4change < c4changes + o.numChanges; c4change++ ) {
                        if ( c4change->sequence <= prevLastSequence ) continue;
                        if ( !(_enumFlags & kC4IncludeDeleted) && (c4change->flags & kDocDeleted) ) continue;
                        C4DocumentInfo info = {};
                        info.sequence       = c4change->sequence;
                        info.docID          = c4change->docID;
                        info.revID          = c4change->revID;
                        info.flags          = c4change->flags;
                        alloc_slice body;
                        if ( _enumFlags & kC4IncludeBodies ) {
                            if ( info.flags & kDocDeleted ) body = R"({"_deleted":true})";
                            else {
                                if ( !coll ) {
                                    tie(db, coll) = listener()->collectionFor(_rq, false);
                                    if ( !coll ) {
                                        stop("collection/database deleted");
                                        return;
                                    }
                                }
                                auto doc = coll->getDocument(info.docID, false);
                                if ( doc && doc->revID() == info.revID ) body = doc->bodyAsJSON();
                                else
                                    continue;  // skip this rev since doc was apparently deleted/updated
                            }
                        }
                        writeChange(info, body);
                    }
                }
                if ( _feed == continuous ) _rq.flush();
                if (_feed == longpoll && _sent > 0)
                    _stopMessage = "longpoll done";
                else if (_rq.socketError())
                    _stopMessage = "peer closed";
            }
            if ( _stopMessage ) { stop(); }
        }

        void heartbeat(chrono::milliseconds interval) {
            c4log(ListenerLog, kC4LogVerbose, "ChangesTask %p heartbeat", this);
            {
                unique_lock lock(_mutex);
                if ( _rq.finished() ) return;
                _rq.write("\n");
                _rq.flush();
                _heartbeatTimer->fireAfter(interval);
                if (_rq.socketError())
                    _stopMessage = "peer closed";
            }
            if ( _stopMessage ) { stop(); }
        }

        void timeout() {
            {
                unique_lock lock(_mutex);
                if ( _rq.finished() ) return;
            }
            stop("timed out");
        }

        RequestResponse                  _rq;              // The HTTP request+response
        string                           _keyspace;        // Scope.collection
        C4SequenceNumber                 _lastSequence{};  // Collection's last sequence number
        uint64_t                         _limit;           // The ?limit param: Max number of changes to return
        C4EnumeratorFlags                _enumFlags;       // Based on ?descending, ?active_only, ?include_docs
        FeedType                         _feed = unknown;  // The ?feed parameter
        unsigned                         _sent = 0;        // Number of change sent so far
        unique_ptr<C4CollectionObserver> _observer;        // Observer, used in async modes
        optional<actor::Timer>           _heartbeatTimer;  // Timer for sending regular newlines
        optional<actor::Timer>           _timeoutTimer;    // Timer for closing connection
        const char* _stopMessage = nullptr;
    };

    void RESTListener::handleChanges(RequestResponse& rq, C4Collection* coll) {
        // Note: This moves the RequestResponse from `rq` to the ChangesTask's `_rq`.
        // That means the Changestask takes ownership of the socket from the calling Server.
        auto task = make_retained<ChangesTask>(this, std::move(rq));
        if (task->runImmediate(coll))
            rq = std::move(*task).extractRequest();
    }
}  // namespace litecore::REST
