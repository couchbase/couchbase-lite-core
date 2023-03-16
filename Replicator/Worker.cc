//
// Worker.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Worker.hh"
#include "Replicator.hh"
#include "ReplicatorTypes.hh"
#include "c4Private.h"
#include "Increment.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/PlatformCompat.hh"
#include "BLIP.hh"
#include "HTTPTypes.hh"
#include <sstream>

#if defined(__clang__) && !defined(__ANDROID__)
#    include <cxxabi.h>
#endif

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore {
    LogDomain SyncLog("Sync");
}

namespace litecore { namespace repl {

    std::unordered_map<std::string, unsigned short> Worker::_formatCache;

    LogDomain SyncBusyLog("SyncBusy", LogLevel::Warning);

    static void writeRedacted(Dict dict, stringstream& s) {
        s << "{";
        int n = 0;
        for ( Dict::iterator i(dict); i; ++i ) {
            if ( n++ > 0 ) s << ", ";
            slice key = i.keyString();
            s << key << ":";
            if ( key == slice(C4STR(kC4ReplicatorAuthPassword)) ) {
                s << "\"********\"";
            } else if ( i.value().asDict() ) {
                writeRedacted(i.value().asDict(), s);
            } else {
                alloc_slice json(i.value().toJSON5());
                s << json;
            }
        }
        s << "}";
    }

    Options::operator string() const {
        static const char* kModeNames[] = {"disabled", "passive", "one-shot", "continuous"};
        stringstream       s;
        s << "{";
        bool     firstline = true;
        uint32_t i         = 0;  // Collection index
        for ( auto& c : collectionOpts ) {
            if ( !firstline ) {
                s << ",\n";
            } else {
                firstline = false;
            }
            s << format(string(kCollectionLogFormat), i++) << " "
              << "\"" << collectionSpecToPath(c.collectionSpec).asString() << "\": {"
              << "\"Push\": " << kModeNames[c.push] << ", "
              << "\"Pull\": " << kModeNames[c.pull] << "}";
        }
        s << "}\n";

        s << "Options=";
        writeRedacted(properties, s);
        return s.str();
    }

    Worker::Worker(blip::Connection* connection, Worker* parent, const Options* options,
                   std::shared_ptr<DBAccess> dbAccess, const char* namePrefix, CollectionIndex coll)
        : Actor(SyncLog, string(namePrefix) + connection->name(), (parent ? parent->mailboxForChildren() : nullptr))
        , _connection(connection)
        , _parent(parent)
        , _options(options)
        , _db(dbAccess)
        , _status{(connection->state() >= Connection::kConnected) ? kC4Idle : kC4Connecting}
        , _loggingID(parent ? parent->replicator()->loggingName() : connection->name())
        , _collectionIndex(coll) {}

    Worker::Worker(Worker* parent, const char* namePrefix, CollectionIndex coll)
        : Worker(&parent->connection(), parent, parent->_options, parent->_db, namePrefix, coll) {}

    Worker::~Worker() {
        if ( _importance ) logStats();
        logDebug("destructing (%p); actorName='%s'", this, actorName().c_str());
    }

    string Worker::loggingClassName() const {
        string className = Logging::loggingClassName();
        if ( passive() ) toLowercase(className);
        return className;
    }

    void Worker::sendRequest(blip::MessageBuilder& builder, MessageProgressCallback callback) {
        if ( callback ) {
            increment(_pendingResponseCount);
            builder.onProgress = asynchronize("sendRequest callback", [=](MessageProgress progress) {
                if ( progress.state >= MessageProgress::kComplete ) decrement(_pendingResponseCount);
                callback(progress);
            });
        } else {
            if ( !builder.noreply ) warn("Ignoring the response to a BLIP message!");
        }
        connection().sendRequest(builder);
    }

#pragma mark - ERRORS:

    blip::ErrorBuf Worker::c4ToBLIPError(C4Error err) {
        if ( !err.code ) return {};
        slice       blipDomain = slice(error::nameOfDomain((error::Domain)err.domain));
        auto        code       = err.code;
        alloc_slice message(err.message());

        //FIX: Map more common errors to standard domains/codes (via table lookup)
        switch ( err.domain ) {
            case LiteCoreDomain:
                if ( err.code == kC4ErrorCorruptDelta || err.code == kC4ErrorDeltaBaseUnknown ) {
                    blipDomain = "HTTP";
                    code       = int(net::HTTPStatus::UnprocessableEntity);
                }
                break;
            case WebSocketDomain:
                if ( err.code < 1000 ) blipDomain = "HTTP";
                break;
            default:
                break;
        }
        return {blipDomain, code, message};
    }

    C4Error Worker::blipToC4Error(const blip::Error& err) {
        if ( !err.domain || err.code == 0 ) return {};
        C4ErrorDomain domain = LiteCoreDomain;
        int           code   = 0;
        if ( err.domain == "HTTP"_sl ) {
            domain = WebSocketDomain;
            code   = err.code;
        } else {
            for ( error::Domain d = error::LiteCore; d < error::NumDomainsPlus1; d = (error::Domain)(d + 1) ) {
                if ( err.domain == slice(error::nameOfDomain(d)) ) {
                    domain = (C4ErrorDomain)d;
                    code   = err.code;
                    break;
                }
            }
        }
        if ( code == 0 ) {
            // Do not log "unknown error" for (BLIP, 404) as we know of that error
            if ( !(err.domain == "BLIP"_sl && err.code == 404) )
                LogWarn(SyncLog, "Received unknown error {'%.*s' %d \"%.*s\"} from server", SPLAT(err.domain), err.code,
                        SPLAT(err.message));
            code = kC4ErrorRemoteError;
        }
        return C4Error::make(domain, code, err.message);
    }

    void Worker::gotError(const MessageIn* msg) {
        auto err = msg->getError();
        logError("Got error response: %.*s %d '%.*s'", SPLAT(err.domain), err.code, SPLAT(err.message));
        onError(blipToC4Error(err));
    }

    void Worker::gotError(C4Error err) {
        logError("Got LiteCore error: %s", err.description().c_str());
        onError(err);
    }

    void Worker::caughtException(const std::exception& x) {
        logError("Threw C++ exception: %s", x.what());
        onError(C4Error::make(LiteCoreDomain, kC4ErrorUnexpectedError, slice(x.what())));
    }

    void Worker::onError(C4Error err) {
        _status.error  = err;
        _statusChanged = true;
    }

    Retained<Replicator> Worker::replicatorIfAny() {
        Retained<Worker> parent = _parent;
        if ( !parent ) return nullptr;
        return parent->replicatorIfAny();
    }

    Retained<Replicator> Worker::replicator() {
        auto replicator = replicatorIfAny();
        Assert(replicator != nullptr);
        return replicator;
    }

    void Worker::finishedDocumentWithError(ReplicatedRev* rev, C4Error error, bool transient) {
        rev->error            = error;
        rev->errorIsTransient = transient;
        finishedDocument(rev);
    }

    void Worker::finishedDocument(ReplicatedRev* rev) {
        if ( rev->error.code == 0 ) addProgress({0, 0, 1});
        if ( rev->error.code || rev->isWarning || progressNotificationLevel() >= 1 ) replicator()->endedDocument(rev);
    }

#pragma mark - ACTIVITY / PROGRESS:

    void Worker::setProgress(C4Progress p) { addProgress(p - _status.progress); }

    void Worker::addProgress(C4Progress p) {
        if ( p.unitsCompleted || p.unitsTotal || p.documentCount ) {
            _status.progressDelta += p;
            _status.progress += p;
            _statusChanged = true;
#if DEBUG
            if ( _status.progress.unitsCompleted > _status.progress.unitsTotal )
                warn("Adding progress %" PRIu64 "/%" PRIu64 " gives invalid result %" PRIu64 "/%" PRIu64 "",
                     p.unitsCompleted, p.unitsTotal, _status.progress.unitsCompleted, _status.progress.unitsTotal);
#endif
        }
    }

    Worker::ActivityLevel Worker::computeActivityLevel() const {
        if ( eventCount() > 1 || _pendingResponseCount > 0 ) return kC4Busy;
        else
            return kC4Idle;
    }

    // Called after every event; updates busy status & detects when I'm done
    void Worker::afterEvent() {
        (void)SyncBusyLog.level();  // initialize its level

        bool changed   = _statusChanged;
        _statusChanged = false;
        if ( changed && _importance ) {
            logVerbose("(collection: %u) progress +%" PRIu64 "/+%" PRIu64 ", %" PRIu64 " docs -- now %" PRIu64
                       " / %" PRIu64 ", %" PRIu64 " docs",
                       collectionIndex(), _status.progressDelta.unitsCompleted, _status.progressDelta.unitsTotal,
                       _status.progressDelta.documentCount, _status.progress.unitsCompleted,
                       _status.progress.unitsTotal, _status.progress.documentCount);
        }

        auto newLevel = computeActivityLevel();
        if ( newLevel != _status.level ) {
            _status.level = newLevel;
            changed       = true;
            if ( _importance ) {
                auto name = kC4ReplicatorActivityLevelNames[newLevel];
                if ( _importance > 1 ) logInfo("now %-s", name);
                else
                    logVerbose("now %-s", name);
            }
        }
        if ( changed ) changedStatus();
        _status.progressDelta = {0, 0};
    }

    void Worker::changedStatus() {
        if ( _parent ) _parent->childChangedStatus(this, _status);
        if ( _status.level == kC4Stopped ) _parent = nullptr;
    }

    // Either there is error, or return a valid collection index
    std::pair<CollectionIndex, slice> Worker::checkCollectionOfMsg(const blip::MessageIn& msg) const {
        CollectionIndex collIn = getCollectionIndex(msg);

        constexpr static slice kErrorIndexInappropriateUse = "inappropriate use of the collection property."_sl;
        constexpr static slice kErrorIndexOutOfRange       = "the collection property is out of range."_sl;

        slice err = nullslice;
        if ( _options->collectionAware() ) {
            if ( collIn == kNotCollectionIndex ) { err = kErrorIndexInappropriateUse; }
        } else {
            if ( collIn != kNotCollectionIndex ) {
                err = kErrorIndexInappropriateUse;
            } else {
                collIn = 0;
            }
        }

        if ( !err && collIn >= _options->workingCollectionCount() ) { err = kErrorIndexOutOfRange; }

        return std::make_pair(collIn, err);
    }

    const C4Collection* Worker::getCollection() const {
        Assert(collectionIndex() != kNotCollectionIndex);
        Worker* nonConstThis = const_cast<Worker*>(this);
        return nonConstThis->replicator()->collection(collectionIndex());
    }
}}  // namespace litecore::repl
