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
#include "Increment.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "HTTPTypes.hh"
#include <sstream>
#include <thread>
#include <utility>

#if defined(__clang__) && !defined(__ANDROID__)
#    include <cxxabi.h>
#endif

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore {
    LogDomain SyncLog("Sync");
}

namespace litecore::repl {

    std::unordered_set<std::string> Worker::_formatCache;
    std::shared_mutex               Worker::_formatMutex;

    LogDomain SyncBusyLog("SyncBusy", LogLevel::Warning);

    static void writeRedacted(Dict dict, stringstream& s) {
        s << "{";
        int n = 0;
        for ( Dict::iterator i(dict); i; ++i ) {
            if ( n++ > 0 ) s << ", ";
            slice key = i.keyString();
            if ( Options::kWhiteListOfKeysToLog.find(key) == Options::kWhiteListOfKeysToLog.end() ) continue;
            s << key << ":";
            if ( i.value().asDict() ) {
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
                s << ", ";
            } else {
                firstline = false;
            }
            s << "{Coll#" << i++ << "}"
              << " "
              << "\"" << collectionSpecToPath(c.collectionSpec).asString() << "\": {"
              << "\"Push\": " << kModeNames[c.push] << ", "
              << "\"Pull\": " << kModeNames[c.pull] << ", "
              << "Options=";
            writeRedacted(c.properties, s);
            s << "}";
        }
        s << "} ";

        s << "Options=";
        writeRedacted(properties, s);
        return s.str();
    }

    Worker::Worker(blip::Connection* connection, Worker* parent, const Options* options,
                   std::shared_ptr<DBAccess> dbAccess, const char* namePrefix, CollectionIndex coll)
        : Actor(SyncLog, string(namePrefix) + connection->name(), (parent ? parent->mailboxForChildren() : nullptr))
        , _options(options)
        , _parent(parent)
        , _db(std::move(dbAccess))
        , _loggingID(parent ? parent->replicator()->loggingName() : connection->name())
        , _connection(connection)
        , _status{(connection->state() >= Connection::kConnected) ? kC4Busy : kC4Connecting}
        , _collectionSpec(coll != kNotCollectionIndex ? replicator()->collectionSpec(coll) : C4CollectionSpec{})
        , _collectionIndex(coll) {
        static std::once_flag f_once;
        std::call_once(f_once, [] {
            // Reserve in the format-string cache greater than the number of unique collection log strings, so
            // rehashing should almost never happen
            _formatCache.reserve(300);
        });
    }

    Worker::Worker(Worker* parent, const char* namePrefix, CollectionIndex coll)
        : Worker(&parent->connection(), parent, parent->_options, parent->_db, namePrefix, coll) {}

    Worker::~Worker() {
        if ( _importance ) logStats();
        logDebug("destructing (%p); actorName='%s'", this, actorName().c_str());
    }

    void Worker::sendRequest(blip::MessageBuilder& builder, const MessageProgressCallback& callback) {
        if ( callback ) {
            increment(_pendingResponseCount);
            builder.onProgress = asynchronize("sendRequest callback", [this, callback](MessageProgress progress) {
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
        auto        blipDomain = slice(error::nameOfDomain((error::Domain)err.domain));
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
        if ( rev->error.code || rev->isWarning || (!rev->alreadyExisted && progressNotificationLevel() >= 1) )
            replicator()->endedDocument(rev);
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

    Worker::ActivityLevel Worker::computeActivityLevel(std::string* reason) const {
        Worker::ActivityLevel level{kC4Idle};
        if ( eventCount() > 1 || _pendingResponseCount > 0 ) level = kC4Busy;
        else
            level = kC4Idle;

        if ( reason ) {
            if ( level == kC4Busy ) {
                if ( eventCount() > 1 ) *reason = stringprintf("pendingEvent/%u", eventCount());
                else
                    *reason = stringprintf("pendingResponse/%d", _pendingResponseCount);
            } else {
                *reason = "noPendingEventOrResponse";
            }
        }

        return level;
    }

    // Called after every event; updates busy status & detects when I'm done
    void Worker::afterEvent() {
        (void)SyncBusyLog.level();  // initialize its level

        bool changed   = _statusChanged;
        _statusChanged = false;
        if ( changed && _importance ) {
            logVerbose("progress +%" PRIu64 "/+%" PRIu64 ", %" PRIu64 " docs -- now %" PRIu64 " / %" PRIu64 ", %" PRIu64
                       " docs",
                       _status.progressDelta.unitsCompleted, _status.progressDelta.unitsTotal,
                       _status.progressDelta.documentCount, _status.progress.unitsCompleted,
                       _status.progress.unitsTotal, _status.progress.documentCount);
        }

        std::string reason;
        auto        newLevel = computeActivityLevel(willLog(LogLevel::Info) ? &reason : nullptr);
        if ( newLevel != _status.level ) {
            auto oldLevel = _status.level;
            _status.level = newLevel;
            changed       = true;
            if ( _importance ) {
                auto oldName = kC4ReplicatorActivityLevelNames[oldLevel];
                auto name    = kC4ReplicatorActivityLevelNames[newLevel];
                if ( _importance > 1 ) {
                    if ( reason.empty() ) logInfo("status=%s from=%s", name, oldName);
                    else
                        logInfo("status=%s from=%s reason=%s", name, oldName, reason.c_str());
                } else {
                    if ( reason.empty() ) logVerbose("status=%s from=%s", name, oldName);
                    else
                        logVerbose("status=%s from=%s reason=%s", name, oldName, reason.c_str());
                }
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
        if ( collIn == kNotCollectionIndex ) { err = kErrorIndexInappropriateUse; }

        if ( !err && collIn >= _options->workingCollectionCount() ) { err = kErrorIndexOutOfRange; }

        return std::make_pair(collIn, err);
    }

    string Worker::loggingKeyValuePairs() const {
        string kv = Actor::loggingKeyValuePairs();
        if ( auto collIdx = collectionIndex(); collIdx != kNotCollectionIndex ) {
            if ( !kv.empty() ) kv += " ";
            kv += "Coll=";
            kv += to_string(collIdx);
        }
        return kv;
    }

    const std::unordered_set<slice> Options::kWhiteListOfKeysToLog{
            // That is, they are supposed to be assigned to c4ReplicatorParameters.collections[i].optionsDictFleece
            kC4ReplicatorOptionDocIDs,
            kC4ReplicatorOptionChannels,
            kC4ReplicatorOptionFilter,
            kC4ReplicatorOptionFilterParams,
            kC4ReplicatorOptionSkipDeleted,
            kC4ReplicatorOptionNoIncomingConflicts,
            // end of collection specific properties.

            kC4ReplicatorCheckpointInterval,
            kC4ReplicatorOptionRemoteDBUniqueID,
            kC4ReplicatorOptionDisableDeltas,
            kC4ReplicatorOptionDisablePropertyDecryption,
            kC4ReplicatorOptionMaxRetries,
            kC4ReplicatorOptionMaxRetryInterval,
            kC4ReplicatorOptionAutoPurge,
            kC4ReplicatorOptionAcceptParentDomainCookies,

            // Tuning options:
            kC4ReplicatorOptionMaxRevsBeingRequested,
            kC4ReplicatorOptionMaxIncomingRevs,
            kC4ReplicatorOptionMaxRevsInFlight,

            // TLS options:
            kC4ReplicatorOptionRootCerts,
            kC4ReplicatorOptionPinnedServerCert,
            kC4ReplicatorOptionOnlySelfSignedServerCert,

            // HTTP options:
            kC4ReplicatorOptionExtraHeaders,
            kC4ReplicatorOptionCookies,
            kC4ReplicatorOptionAuthentication,
            kC4ReplicatorOptionProxyServer,

            // WebSocket options:
            kC4ReplicatorHeartbeatInterval,
            kC4SocketOptionWSProtocols,
            kC4SocketOptionNetworkInterface,

            // BLIP options:
            kC4ReplicatorCompressionLevel,

            // [1]: Auth dictionary keys:
            kC4ReplicatorAuthType,
            kC4ReplicatorAuthUserName,
            // kC4ReplicatorAuthPassword,
            kC4ReplicatorAuthEnableChallengeAuth,
            kC4ReplicatorAuthClientCert,
            // kC4ReplicatorAuthClientCertKey,
            // kC4ReplicatorAuthToken,

            // [3]: Proxy dictionary keys:
            kC4ReplicatorProxyType,
            kC4ReplicatorProxyHost,
            kC4ReplicatorProxyPort,
            kC4ReplicatorProxyAuth,
    };
}  // namespace litecore::repl
