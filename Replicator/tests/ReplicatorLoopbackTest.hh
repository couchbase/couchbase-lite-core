//
//  ReplicatorLoopbackTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright 2017-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "fleece/Fleece.hh"
#include "c4CppUtils.hh"
#include "c4BlobStore.h"
#include "c4Collection.h"
#include "c4DocEnumerator.h"
#include "c4Document+Fleece.h"
#include "Replicator.hh"
#include "Checkpoint.hh"
#include "LoopbackProvider.hh"
#include "ReplicatorTuning.hh"
#include "StringUtil.hh"
#include "SecureRandomize.hh"
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "c4Test.hh"


using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;

class ReplicatorLoopbackTest
    : public C4Test
    , Replicator::Delegate {
  public:
    using duration = std::chrono::nanoseconds;

    static constexpr duration kLatency = 50ms;

    slice kNonLocalRev1ID, kNonLocalRev2ID, kNonLocalRev3ID, kConflictRev2AID, kConflictRev2BID;

#if SkipVersionVectorTest
    static constexpr int numberOfOptions = 1;
#else
    static constexpr int numberOfOptions = 2;
#endif

    ReplicatorLoopbackTest(int which) : C4Test(which), db2(createDatabase("2")) {
        // Change tuning param so that tests will actually create deltas, despite using small
        // document bodies:
        litecore::repl::tuning::kMinBodySizeForDelta = 0;
        litecore::repl::Checkpoint::gWriteTimestamps = false;
        _clientProgressLevel = _serverProgressLevel = kC4ReplProgressOverall;

        _collDB1 = createCollection(db, _collSpec);
        _collDB2 = createCollection(db2, _collSpec);

        if ( isRevTrees() ) {
            kNonLocalRev1ID  = kRev1ID;
            kNonLocalRev2ID  = kRev2ID;
            kNonLocalRev3ID  = kRev3ID;
            kConflictRev2AID = "2-2a2a2a2a"_sl;
            kConflictRev2BID = "2-2b2b2b2b"_sl;
        } else {
            kNonLocalRev1ID  = "1@SarahCynthiaSylviaStow"_sl;
            kNonLocalRev2ID  = "2@SarahCynthiaSylviaStow"_sl;
            kNonLocalRev3ID  = "3@SarahCynthiaSylviaStow"_sl;
            kConflictRev2AID = "1@NorbertHeisenbergVonQQ"_sl;
            kConflictRev2BID = "1@MajorMajorMajorMajorQQ"_sl;
        }
    }

    ~ReplicatorLoopbackTest() override {
        CHECK(!_parallelThread);
        _replClient = _replServer = nullptr;
        C4Error error;
        CHECK(c4db_delete(db2, WITH_ERROR(&error)));
        c4db_release(db2);
    }

    // opts1 is the options for _db; opts2 is the options for _db2
    void runReplicators(const Replicator::Options& opts1, const Replicator::Options& opts2, bool reset = false) {
        std::unique_lock<std::mutex> lock(_mutex);

        _gotResponse              = false;
        _statusChangedCalls       = 0;
        _statusReceived           = Worker::Status();
        _replicatorClientFinished = _replicatorServerFinished = false;

        c4::ref<C4Database> dbClient = c4db_openAgain(db, nullptr);
        c4::ref<C4Database> dbServer = c4db_openAgain(db2, nullptr);
        REQUIRE(dbClient);
        REQUIRE(dbServer);

        auto optsRef1 = make_retained<Replicator::Options>(opts1);
        auto optsRef2 = make_retained<Replicator::Options>(opts2);
        if ( optsRef2->collectionCount() > 0 && (optsRef2->push(0) > kC4Passive || optsRef2->pull(0) > kC4Passive) ) {
            // always make opts1 the active (client) side
            std::swap(dbServer, dbClient);
            std::swap(optsRef1, optsRef2);
            std::swap(_clientProgressLevel, _serverProgressLevel);
        }
        optsRef1->setProgressLevel(_clientProgressLevel);
        optsRef2->setProgressLevel(_serverProgressLevel);

        bool createReplicatorThrew = false;
        // Create client (active) and server (passive) replicators:
        try {
            if ( _updateClientOptions ) { optsRef1 = make_retained<repl::Options>(_updateClientOptions(*optsRef1)); }
            _replClient =
                    new Replicator(dbClient, new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Client, kLatency),
                                   *this, optsRef1);

            _replServer =
                    new Replicator(dbServer, new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Server, kLatency),
                                   *this, optsRef2);

            Log("Client replicator is %s", _replClient->loggingName().c_str());

            // Response headers:
            Headers headers;
            headers.add("Set-Cookie"_sl, "flavor=chocolate-chip"_sl);

            // Bind the replicators' WebSockets and start them:
            LoopbackWebSocket::bind(_replClient->webSocket(), _replServer->webSocket(), headers);
            Stopwatch st;
            _replClient->start(reset);
            _replServer->start();

            Log("Waiting for replication to complete...");
            static constexpr size_t timeoutMins = 5;  // Number of minutes to timeout after
            _cond.wait_for(lock, std::chrono::minutes(timeoutMins),
                           [&] { return _replicatorClientFinished && _replicatorServerFinished; });
            if ( !(_replicatorClientFinished && _replicatorServerFinished) ) {
                FAIL("Replication timed out after " << timeoutMins << " minutes...");
            }

            if ( _parallelThread ) {
                _parallelThread->join();
                _parallelThread = nullptr;
            }

            Log(">>> Replication complete (%.3f sec) <<<", st.elapsed());

            _checkpointIDs.clear();
            for ( int i = 0; i < optsRef1->collectionOpts.size(); ++i ) {
                _checkpointIDs.push_back(_replClient->checkpointer(i).checkpointID());
            }
        } catch ( const exception& exc ) {
            // In this suite of tests, we don't use C4Replicator as the holder.
            // The delegate of the respective Replicators is 'this'. With C4Replicator
            // as the holder, the exception would have been caught by C4Replicator when it
            // calls createReplicator(). We try to match that logic here.
            createReplicatorThrew = true;
            _statusReceived.error = C4Error::fromException(exc);
        }
        _replClient = _replServer = nullptr;

        CHECK((createReplicatorThrew || _gotResponse));
        CHECK((createReplicatorThrew || _statusChangedCalls > 0));
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK((_statusReceived.error == _expectedError || _ignoreStatusError));
        if ( !(_ignoreLackOfDocErrors && _docPullErrors.empty()) )
            CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        if ( !(_ignoreLackOfDocErrors && _docPushErrors.empty()) )
            CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
        if ( _checkDocsFinished ) CHECK(asVector(_docsFinished) == asVector(_expectedDocsFinished));
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if ( _expectedUnitsComplete >= 0 ) CHECK(_expectedUnitsComplete == _statusReceived.progress.unitsCompleted);
        if ( _expectedDocumentCount >= 0 )
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
    }

    bool runReplicatorsAsync(const Replicator::Options& opts1, const Replicator::Options& opts2) {
        _gotResponse              = false;
        _statusChangedCalls       = 0;
        _statusReceived           = Worker::Status();
        _replicatorClientFinished = _replicatorServerFinished = false;

        c4::ref<C4Database> dbClient = c4db_openAgain(db, nullptr);
        c4::ref<C4Database> dbServer = c4db_openAgain(db2, nullptr);
        REQUIRE(dbClient);
        REQUIRE(dbServer);

        auto optsRef1 = make_retained<Replicator::Options>(opts1);
        auto optsRef2 = make_retained<Replicator::Options>(opts2);
        if ( optsRef2->collectionCount() > 0 && (optsRef2->push(0) > kC4Passive || optsRef2->pull(0) > kC4Passive) ) {
            // always make opts1 the active (client) side
            std::swap(dbServer, dbClient);
            std::swap(optsRef1, optsRef2);
            std::swap(_clientProgressLevel, _serverProgressLevel);
        }
        optsRef1->setProgressLevel(_clientProgressLevel);
        optsRef2->setProgressLevel(_serverProgressLevel);

        bool createReplicatorSucceeded = true;
        // Create client (active) and server (passive) replicators:
        try {
            if ( _updateClientOptions ) { optsRef1 = make_retained<repl::Options>(_updateClientOptions(*optsRef1)); }
            _replClient =
                    new Replicator(dbClient, new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Client, kLatency),
                                   *this, optsRef1);

            _replServer =
                    new Replicator(dbServer, new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Server, kLatency),
                                   *this, optsRef2);

            Log("Client replicator is %s", _replClient->loggingName().c_str());

            // Response headers:
            Headers headers;
            headers.add("Set-Cookie"_sl, "flavor=chocolate-chip"_sl);

            // Bind the replicators' WebSockets and start them:
            LoopbackWebSocket::bind(_replClient->webSocket(), _replServer->webSocket(), headers);
            _replClient->start();
            _replServer->start();
        } catch ( const exception& exc ) {
            // In this suite of tests, we don't use C4Replicator as the holder.
            // The delegate of the respective Replicators is 'this'. With C4Replicator
            // as the holder, the exception would have been caught by C4Replicator when it
            // calls createReplicator(). We try to match that logic here.
            createReplicatorSucceeded = false;
            _statusReceived.error     = C4Error::fromException(exc);
        }
        return createReplicatorSucceeded;
    }

    void waitForReplicators(const Replicator::Options& opts1, const Replicator::Options& opts2) {
        std::unique_lock lock(_mutex);
        auto             optsRef1 = make_retained<Replicator::Options>(opts1);
        auto             optsRef2 = make_retained<Replicator::Options>(opts2);
        if ( optsRef2->collectionCount() > 0 && (optsRef2->push(0) > kC4Passive || optsRef2->pull(0) > kC4Passive) ) {
            // always make opts1 the active (client) side
            std::swap(optsRef1, optsRef2);
        }
        Stopwatch st;
        Log("Waiting for replication to complete...");
        static constexpr size_t timeoutMins = 5;  // Number of minutes to timeout after
        _cond.wait_for(lock, std::chrono::minutes(timeoutMins),
                       [&] { return _replicatorClientFinished && _replicatorServerFinished; });
        if ( !(_replicatorClientFinished && _replicatorServerFinished) ) {
            FAIL("Replication timed out after " << timeoutMins << " minutes...");
        }

        Log(">>> Replication complete (%.3f sec) <<<", st.elapsed());

        _checkpointIDs.clear();
        for ( int i = 0; i < optsRef1->collectionOpts.size(); ++i ) {
            _checkpointIDs.push_back(_replClient->checkpointer(i).checkpointID());
        }

        _replClient = _replServer = nullptr;

        CHECK(_gotResponse);
        CHECK(_statusChangedCalls > 0);
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK(_statusReceived.error.code == _expectedError.code);
        if ( _expectedError.code ) CHECK(_statusReceived.error.domain == _expectedError.domain);
        if ( !(_ignoreLackOfDocErrors && _docPullErrors.empty()) )
            CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        if ( !(_ignoreLackOfDocErrors && _docPushErrors.empty()) )
            CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
        if ( _checkDocsFinished ) CHECK(asVector(_docsFinished) == asVector(_expectedDocsFinished));
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if ( _expectedUnitsComplete >= 0 ) CHECK(_expectedUnitsComplete == _statusReceived.progress.unitsCompleted);
        if ( _expectedDocumentCount >= 0 )
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
    }

    void runPushReplication(C4ReplicatorMode mode = kC4OneShot) {
        runReplicators(Replicator::Options::pushing(mode, _collSpec), Replicator::Options::passive(_collSpec));
    }

    void runPullReplication(C4ReplicatorMode mode = kC4OneShot) {
        runReplicators(Replicator::Options::passive(_collSpec), Replicator::Options::pulling(mode, _collSpec));
    }

    void runPushPullReplication(C4ReplicatorMode mode = kC4OneShot) {
        runReplicators(Replicator::Options::pushpull(mode, _collSpec), Replicator::Options::passive(_collSpec));
    }

    void stopWhenIdle() {
        std::lock_guard<std::mutex> lock(_mutex);
        if ( !_stopOnIdle ) {
            _stopOnIdle = true;
            if ( !_checkStopWhenIdle() ) Log(">>    Will stop replicator when idle...");
        }
    }

    // must be holding _mutex to call this
    bool _checkStopWhenIdle() {
        if ( _stopOnIdle && _statusReceived.level == kC4Idle ) {
            if ( _conflictHandlerRunning ) {
                Log(">>    Conflict resolution active, delaying stop...");
                return false;
            }

            Log(">>    Stopping idle replicator...");
            _replClient->stop();
            return true;
        }
        return false;
    }

    std::function<void()> _callbackWhenIdle;


#pragma mark - CALLBACKS:

    void replicatorGotTLSCertificate(slice certData) override {}

    void replicatorStatusChanged(Replicator* repl, const Replicator::Status& status) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if ( repl == _replClient ) {
            if ( !_gotResponse ) {
                _gotResponse               = true;
                auto [httpStatus, headers] = repl->httpResponse();
                Check(httpStatus == 200);
                Check(headers["Set-Cookie"_sl] == "flavor=chocolate-chip"_sl);
            }

            ++_statusChangedCalls;
            Log(">> Replicator is %-s, progress %lu/%lu, %lu docs", kC4ReplicatorActivityLevelNames[status.level],
                (unsigned long)status.progress.unitsCompleted, (unsigned long)status.progress.unitsTotal,
                (unsigned long)status.progress.documentCount);
            Check(status.progress.unitsCompleted <= status.progress.unitsTotal);
            Check(status.progress.documentCount < 1000000);
            if ( status.progress.unitsTotal > 0 ) {
                Check(status.progress.unitsCompleted >= _statusReceived.progress.unitsCompleted);
                Check(status.progress.unitsTotal >= _statusReceived.progress.unitsTotal);
                Check(status.progress.documentCount >= _statusReceived.progress.documentCount);
            }

            {
                _statusReceived = status;
                _checkStopWhenIdle();
                if ( _statusReceived.level == kC4Idle && _callbackWhenIdle ) { _callbackWhenIdle(); }
            }
        }

        bool& finished = (repl == _replClient) ? _replicatorClientFinished : _replicatorServerFinished;
        Check(!finished);
        if ( status.level == kC4Stopped ) {
            finished = true;
            if ( _replicatorClientFinished && _replicatorServerFinished ) _cond.notify_all();
        }
    }

    void replicatorDocumentsEnded(Replicator* repl, const std::vector<Retained<ReplicatedRev>>& revs) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if ( repl == _replClient ) {
            Check(!_replicatorClientFinished);
            for ( auto& rev : revs ) {
                auto dir = rev->dir();
                if ( rev->error.code ) {
                    if ( dir == Dir::kPulling && rev->error.domain == LiteCoreDomain
                         && rev->error.code == kC4ErrorConflict && _conflictHandler ) {
                        Log(">> Replicator pull conflict for '%.*s'", SPLAT(rev->docID));
                        _conflictHandler(rev);
                    } else {
                        Log(">> Replicator %serror %s '%.*s' #%.*s: %s", (rev->errorIsTransient ? "transient " : ""),
                            (dir == Dir::kPushing ? "pushing" : "pulling"), SPLAT(rev->docID), SPLAT(rev->revID),
                            rev->error.description().c_str());
                        if ( !rev->errorIsTransient || !_ignoreTransientErrors ) {
                            if ( dir == Dir::kPushing ) _docPushErrors.emplace(rev->docID);
                            else
                                _docPullErrors.emplace(rev->docID);
                        }
                    }
                } else {
                    Log(">> Replicator %s '%.*s' #%.*s", (dir == Dir::kPushing ? "pushed" : "pulled"),
                        SPLAT(rev->docID), SPLAT(rev->revID));
                    _docsFinished.emplace(rev->docID);
                }
            }
        }
    }

    void replicatorBlobProgress(Replicator* repl, const Replicator::BlobProgress& p) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if ( p.dir == Dir::kPushing ) {
            ++_blobPushProgressCallbacks;
            _lastBlobPushProgress = p;
        } else {
            ++_blobPullProgressCallbacks;
            _lastBlobPullProgress = p;
        }
        alloc_slice keyString(c4blob_keyToString(p.key));
        Log(">> Replicator %s blob '%.*s'%.*s [%.*s] (%" PRIu64 " / %" PRIu64 ")",
            (p.dir == Dir::kPushing ? "pushing" : "pulling"), SPLAT(p.docID), SPLAT(p.docProperty), SPLAT(keyString),
            p.bytesCompleted, p.bytesTotal);
    }

    void replicatorConnectionClosed(Replicator* repl, const CloseStatus& status) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if ( repl == _replClient ) {
            Log(">> Replicator closed with code=%d/%d, message=%.*s", status.reason, status.code,
                SPLAT(status.message));
        }
    }

#pragma mark - CONFLICT HANDLING

    // Installs a simple conflict handler equivalent to the one in CBL 2.0-2.5: it just picks one
    // side and tosses the other one out.
    void installConflictHandler() {
        c4::ref<C4Database> resolvDB = c4db_openAgain(db, nullptr);
        Require(resolvDB);
        auto& conflictHandlerRunning = _conflictHandlerRunning;
        _conflictHandler             = [resolvDB, &conflictHandlerRunning](ReplicatedRev* rev) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            Log("Resolving conflict in '%.*s' %.*s ...", SPLAT(rev->docID), SPLAT(rev->revID));

            conflictHandlerRunning = true;
            TransactionHelper t(resolvDB);
            C4Error           error;
            // Get the local rev:
            auto                collection = c4db_getCollection(resolvDB, rev->collectionSpec, ERROR_INFO());
            c4::ref<C4Document> doc        = c4coll_getDoc(collection, rev->docID, true, kDocGetAll, &error);
            if ( !doc ) {
                WarnError("conflictHandler: Couldn't read doc '%.*s'", SPLAT(rev->docID));
                Require(doc);
            }
            if ( !(doc->flags & kDocConflicted) ) {
                Log("conflictHandler: Doc '%.*s' not conflicted anymore (at %.*s)", SPLAT(rev->docID),
                                SPLAT(doc->revID));
                conflictHandlerRunning = false;
                return;
            }
            alloc_slice     localRevID = doc->selectedRev.revID;
            C4RevisionFlags localFlags = doc->selectedRev.flags;
            FLDict          localBody  = c4doc_getProperties(doc);
            // Get the remote rev:
            Require(c4doc_selectNextLeafRevision(doc, true, false, &error));
            alloc_slice     remoteRevID = doc->selectedRev.revID;
            C4RevisionFlags remoteFlags = doc->selectedRev.flags;
            Require(!c4doc_selectNextLeafRevision(doc, true, false, &error));  // no 3rd branch!

            bool remoteWins = false;

            // Figure out which branch should win:
            if ( (localFlags & kRevDeleted) != (remoteFlags & kRevDeleted) )
                remoteWins = (localFlags & kRevDeleted) == 0;  // deletion wins conflict
            else if ( c4rev_getTimestamp(localRevID) != c4rev_getTimestamp(remoteRevID) )
                remoteWins = c4rev_getTimestamp(localRevID) < c4rev_getTimestamp(remoteRevID);

            Log("Resolving conflict in '%.*s': local=#%.*s (%02X), remote=#%.*s (%02X); %s wins", SPLAT(rev->docID),
                            SPLAT(localRevID), localFlags, SPLAT(remoteRevID), remoteFlags, (remoteWins ? "remote" : "local"));

            // Resolve. The remote rev has to win, in that it has to stay on the main branch, to avoid
            // conflicts with the server. But if we want the local copy to really win, we use its body:
            FLDict          mergedBody  = nullptr;
            C4RevisionFlags mergedFlags = remoteFlags;
            if ( remoteWins ) {
                mergedBody  = localBody;
                mergedFlags = localFlags;
            }
            Check(c4doc_resolveConflict2(doc, remoteRevID, localRevID, mergedBody, mergedFlags, &error));
            Check((doc->flags & kDocConflicted) == 0);
            Require(c4doc_save(doc, 0, &error));
            conflictHandlerRunning = false;
        };
    }

#pragma mark - ADDING DOCS/REVISIONS:

    // Pause the current thread for an interval. If the interval is negative, it will randomize.
    static void sleepFor(duration interval) {
        auto ticks = interval.count();
        if ( ticks < 0 ) {
            ticks    = RandomNumber(uint32_t(-ticks)) + RandomNumber(uint32_t(-ticks));
            interval = duration(ticks);
        }
        std::this_thread::sleep_for(interval);
    }

    int addDocs(C4Collection* coll, duration interval, int total) {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        int              docNo   = 1;
        constexpr size_t bufSize = 20;
        for ( int i = 1; docNo <= total; i++ ) {
            sleepFor(interval);
            Log("-------- Creating %d docs --------", 2 * i);
            TransactionHelper t(db);
            for ( int j = 0; j < 2 * i; j++ ) {
                char docID[bufSize];
                snprintf(docID, bufSize, "newdoc%d", docNo++);
                createRev(coll, c4str(docID), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
            }
        }
        Log("-------- Done creating docs --------");
        return docNo - 1;
    }

    static void addRevs(C4Collection* collection, duration interval, const alloc_slice& docID, int firstRev,
                        int totalRevs, bool useFakeRevIDs, const char* logName) {
        C4Database* db = c4coll_getDatabase(collection);
        for ( int i = 0; i < totalRevs; i++ ) {
            int revNo = firstRev + i;
            sleepFor(interval);
            TransactionHelper t(db);
            string            revID;
            if ( useFakeRevIDs ) {
                revID = isRevTrees(db) ? stringprintf("%d-ffff", revNo) : stringprintf("%d@*", revNo);
                createRev(collection, docID, slice(revID), alloc_slice(kFleeceBody));
            } else {
                string json = stringprintf(R"({"db":"%p","i":%d})", db, revNo);
                revID       = createFleeceRev(collection, docID, nullslice, slice(json));
            }
            unsigned long long sequence = collection->getLastSequence();
            Log("-------- %s %d: Created rev '%.*s' %s (seq #%llu) --------", logName, revNo, SPLAT(docID),
                revID.c_str(), sequence);
        }
        Log("-------- %s: Done creating revs --------", logName);
    }

    static std::thread* runInParallel(const std::function<void()>& callback) {
        return new std::thread([=]() mutable { callback(); });
    }

    void addDocsInParallel(duration interval, int total) {
        _parallelThread.reset(runInParallel([this, interval, total]() {
            _expectedDocumentCount = addDocs(_collDB1, interval, total);
            sleepFor(1s);  // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }

    void addRevsInParallel(duration interval, const alloc_slice& docID, int firstRev, int totalRevs,
                           bool useFakeRevIDs = true) {
        _parallelThread.reset(runInParallel([this, interval, docID, firstRev, totalRevs, useFakeRevIDs]() {
            addRevs(_collDB1, interval, docID, firstRev, totalRevs, useFakeRevIDs, "db");
            sleepFor(1s);  // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }

#pragma mark - VALIDATION:

    alloc_slice absoluteRevID(C4Document* doc) {
        if ( c4rev_getGeneration(doc->revID) > 0 ) return doc->revID;  // legacy tree-based ID
        else
            return c4doc_getRevisionHistory(doc, 999, nullptr, 0);  // version vector
    }

#define fastREQUIRE(EXPR)                                                                                              \
    if ( EXPR )                                                                                                        \
        ;                                                                                                              \
    else                                                                                                               \
        REQUIRE(EXPR)  // REQUIRE() is kind of expensive

    void compareDocs(C4Document* doc1, C4Document* doc2) {
        const auto kPublicDocumentFlags = (kDocDeleted | kDocConflicted | kDocHasAttachments);

        fastREQUIRE(doc1->docID == doc2->docID);
        fastREQUIRE(absoluteRevID(doc1) == absoluteRevID(doc2));
        fastREQUIRE((doc1->flags & kPublicDocumentFlags) == (doc2->flags & kPublicDocumentFlags));

        // Compare canonical JSON forms of both docs:
        Dict rev1 = c4doc_getProperties(doc1), rev2 = c4doc_getProperties(doc2);
        if ( !rev1.isEqual(rev2) ) {  // fast check to avoid expensive toJSON
            alloc_slice json1 = rev1.toJSON(true, true);
            alloc_slice json2 = rev2.toJSON(true, true);
            CHECK(json1 == json2);
        }
    }

    void compareDatabases(bool db2MayHaveMoreDocs = false, bool compareDeletedDocs = true) {
        Assert(OnMainThread());
        C4Log(">> Comparing databases...");
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if ( compareDeletedDocs ) options.flags |= kC4IncludeDeleted;
        C4Error                  error;
        c4::ref<C4DocEnumerator> e1 = c4coll_enumerateAllDocs(_collDB1, &options, ERROR_INFO(error));
        REQUIRE(e1);
        c4::ref<C4DocEnumerator> e2 = c4coll_enumerateAllDocs(_collDB2, &options, ERROR_INFO(error));
        REQUIRE(e2);

        unsigned i = 0;
        while ( c4enum_next(e1, ERROR_INFO(error)) ) {
            c4::ref<C4Document> doc1 = c4enum_getDocument(e1, ERROR_INFO(error));
            fastREQUIRE(doc1);
            INFO("db document #" << i << ": '" << slice(doc1->docID).asString() << "'");
            bool ok = c4enum_next(e2, ERROR_INFO(error));
            fastREQUIRE(ok);
            c4::ref<C4Document> doc2 = c4enum_getDocument(e2, ERROR_INFO(error));
            fastREQUIRE(doc2);
            compareDocs(doc1, doc2);
            ++i;
        }
        REQUIRE(error.code == 0);
        if ( !db2MayHaveMoreDocs ) {
            REQUIRE(!c4enum_next(e2, ERROR_INFO(error)));
            REQUIRE(error.code == 0);
        }
    }

    void validateCheckpoint(C4Database* database, bool local, const char* body, const char* meta = "1-") {
        validateCollectionCheckpoint(database, 0, local, body, meta);
    }

    void validateCheckpoints(C4Database* localDB, C4Database* remoteDB, const char* body, const char* meta = "1-cc") {
        validateCollectionCheckpoints(localDB, remoteDB, 0, body, meta);
    }

    void clearCheckpoint(C4Database* database, bool local) { clearCollectionCheckpoint(database, 0, local); }

    void validateCollectionCheckpoint(C4Database* database, unsigned collectionIndex, bool local, const char* body,
                                      const char* meta = "1-") {
        C4Error err = {};
        C4Slice storeName;
        if ( local ) {
            storeName = C4STR("checkpoints");
        } else {
            storeName = C4STR("peerCheckpoints");
        }

        REQUIRE(collectionIndex < _checkpointIDs.size());
        alloc_slice            checkpointID = _checkpointIDs[collectionIndex];
        c4::ref<C4RawDocument> doc(c4raw_get(database, storeName, checkpointID, WITH_ERROR(&err)));
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << string(checkpointID));
        REQUIRE(doc);
        CHECK(doc->body == c4str(body));
        if ( !local ) CHECK(c4rev_getGeneration(doc->meta) >= c4rev_getGeneration(c4str(meta)));
    }

    void validateCollectionCheckpoints(C4Database* localDB, C4Database* remoteDB, unsigned collectionIndex,
                                       const char* body, const char* meta = "1-cc") {
        validateCollectionCheckpoint(localDB, collectionIndex, true, body, meta);
        validateCollectionCheckpoint(remoteDB, collectionIndex, false, body, meta);
    }

    void clearCollectionCheckpoint(C4Database* database, unsigned collectionIndex, bool local) {
        C4Error err;
        C4Slice storeName;
        if ( local ) {
            storeName = C4STR("checkpoints");
        } else {
            storeName = C4STR("peerCheckpoints");
        }

        REQUIRE(c4raw_put(database, storeName, _checkpointIDs[collectionIndex], kC4SliceNull, kC4SliceNull,
                          ERROR_INFO(&err)));
    }

#pragma mark - Property Encryption:

    static alloc_slice UnbreakableEncryption(slice cleartext, int8_t delta) {
        alloc_slice ciphertext(cleartext);
        for ( size_t i = 0; i < ciphertext.size; ++i )
            (uint8_t&)ciphertext[i] += delta;  // "I've got patent pending on that!" --Wallace
        return ciphertext;
    }

#pragma mark - UTILS:

    template <class SET>
    static std::vector<std::string> asVector(const SET& strings) {
        std::vector<std::string> out;
        out.reserve(strings.size());
        out.reserve(strings.size());
        for ( const std::string& s : strings ) out.push_back(s);
        return out;
    }

#pragma mark - VARS:


    C4Database*                         db2{nullptr};
    static constexpr C4CollectionSpec   _collSpec{"test"_sl, "loopback"_sl};
    C4Collection*                       _collDB1;
    C4Collection*                       _collDB2;
    Retained<Replicator>                _replClient, _replServer;
    std::vector<alloc_slice>            _checkpointIDs;
    std::unique_ptr<std::thread>        _parallelThread;
    bool                                _stopOnIdle{false};
    std::mutex                          _mutex;
    std::condition_variable             _cond;
    bool                                _replicatorClientFinished{false}, _replicatorServerFinished{false};
    C4ReplicatorProgressLevel           _clientProgressLevel{}, _serverProgressLevel{};
    bool                                _gotResponse{false};
    Replicator::Status                  _statusReceived{};
    unsigned                            _statusChangedCalls{0};
    int64_t                             _expectedDocumentCount{0};
    int64_t                             _expectedUnitsComplete{-1};
    C4Error                             _expectedError{};
    std::set<std::string>               _docPushErrors, _docPullErrors;
    std::set<std::string>               _expectedDocPushErrors, _expectedDocPullErrors;
    bool                                _ignoreLackOfDocErrors = false;
    bool                                _ignoreTransientErrors = false;
    bool                                _ignoreStatusError     = false;
    bool                                _checkDocsFinished{true};
    std::multiset<std::string>          _docsFinished, _expectedDocsFinished;
    unsigned                            _blobPushProgressCallbacks{0}, _blobPullProgressCallbacks{0};
    Replicator::BlobProgress            _lastBlobPushProgress{}, _lastBlobPullProgress{};
    std::function<void(ReplicatedRev*)> _conflictHandler;
    bool                                _conflictHandlerRunning{false};
    std::function<repl::Options(const repl::Options&)> _updateClientOptions;
};
