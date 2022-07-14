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

class ReplicatorLoopbackTest : public C4Test, Replicator::Delegate {
public:
    using duration = std::chrono::nanoseconds;

    static constexpr duration kLatency              = 50ms;

    slice kNonLocalRev1ID, kNonLocalRev2ID, kNonLocalRev3ID, kConflictRev2AID, kConflictRev2BID;

    ReplicatorLoopbackTest()
#if SkipVersionVectorTest
    :C4Test(0)
#else
    :C4Test(GENERATE(0, 1))
#endif
    ,db2(createDatabase("2"))
    {
        // Change tuning param so that tests will actually create deltas, despite using small
        // document bodies:
        litecore::repl::tuning::kMinBodySizeForDelta = 0;
        litecore::repl::Checkpoint::gWriteTimestamps = false;
        _clientProgressLevel = _serverProgressLevel = kC4ReplProgressOverall;
        
        if (isRevTrees()) {
            kNonLocalRev1ID = kRev1ID;
            kNonLocalRev2ID = kRev2ID;
            kNonLocalRev3ID = kRev3ID;
            kConflictRev2AID = "2-2a2a2a2a"_sl;
            kConflictRev2BID = "2-2b2b2b2b"_sl;
        } else {
            kNonLocalRev1ID = "1@cafe"_sl;
            kNonLocalRev2ID = "2@cafe"_sl;
            kNonLocalRev3ID = "3@cafe"_sl;
            kConflictRev2AID = "1@babe1"_sl;
            kConflictRev2BID = "1@babe2"_sl;
        }
    }

    ~ReplicatorLoopbackTest() {
        if (_parallelThread)
            _parallelThread->join();
        _replClient = _replServer = nullptr;
        C4Error error;
        REQUIRE(c4db_delete(db2, WITH_ERROR(&error)));
        c4db_release(db2);
    }

    // opts1 is the options for _db; opts2 is the options for _db2
    void runReplicators(Replicator::Options opts1,
                        Replicator::Options opts2,
                        bool reset = false)
    {
        std::unique_lock<std::mutex> lock(_mutex);

        _gotResponse = false;
        _statusChangedCalls = 0;
        _statusReceived = {};
        _replicatorClientFinished = _replicatorServerFinished = false;

        c4::ref<C4Database> dbClient = c4db_openAgain(db, nullptr);
        c4::ref<C4Database> dbServer = c4db_openAgain(db2, nullptr);
        REQUIRE(dbClient);
        REQUIRE(dbServer);

        auto optsRef1 = make_retained<Replicator::Options>(opts1);
        auto optsRef2 = make_retained<Replicator::Options>(opts2);
        if (optsRef2->pushOf(0) > kC4Passive || optsRef2->pullOf(0) > kC4Passive) {
            // always make opts1 the active (client) side
            std::swap(dbServer, dbClient);
            std::swap(optsRef1, optsRef2);
            std::swap(_clientProgressLevel, _serverProgressLevel);
        }
        optsRef1->setProgressLevel(_clientProgressLevel);
        optsRef2->setProgressLevel(_serverProgressLevel);

        // Create client (active) and server (passive) replicators:
        _replClient = new Replicator(dbClient,
                                     new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Client, kLatency),
                                     *this, optsRef1);

        _replServer = new Replicator(dbServer,
                                     new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Server, kLatency),
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
        _cond.wait(lock, [&]{return _replicatorClientFinished && _replicatorServerFinished;});

        Log(">>> Replication complete (%.3f sec) <<<", st.elapsed());
        
        _checkpointIDs.clear();
        for (int i = 0; i < opts1.collectionOpts.size(); ++i) {
            _checkpointIDs.push_back(_replClient->checkpointer(i).checkpointID());
        }
        _replClient = _replServer = nullptr;

        CHECK(_gotResponse);
        CHECK(_statusChangedCalls > 0);
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK(_statusReceived.error.code == _expectedError.code);
        if (_expectedError.code)
            CHECK(_statusReceived.error.domain == _expectedError.domain);
        if (!(_ignoreLackOfDocErrors &&_docPullErrors.empty()))
            CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        if (!(_ignoreLackOfDocErrors &&_docPushErrors.empty()))
            CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
        if (_checkDocsFinished)
            CHECK(asVector(_docsFinished) == asVector(_expectedDocsFinished));
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if(_expectedUnitsComplete >= 0)
            CHECK(_expectedUnitsComplete == _statusReceived.progress.unitsCompleted);
        if (_expectedDocumentCount >= 0)
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
    }

    void runPushReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options::pushing(mode), Replicator::Options::passive());
    }

    void runPullReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options::passive(), Replicator::Options::pulling(mode));
    }

    void runPushPullReplication(C4ReplicatorMode mode =kC4OneShot) {
        runReplicators(Replicator::Options(mode, mode), Replicator::Options::passive());
    }


    void stopWhenIdle() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_stopOnIdle) {
            _stopOnIdle = true;
            if (!_checkStopWhenIdle())
                Log(">>    Will stop replicator when idle...");
        }
    }

    // must be holding _mutex to call this
    bool _checkStopWhenIdle() {
        if (_stopOnIdle && _statusReceived.level == kC4Idle) {
            if(_conflictHandlerRunning) {
                Log(">>    Conflict resolution active, delaying stop...");
                return false;
            }

            Log(">>    Stopping idle replicator...");
            _replClient->stop();
            return true;
        }
        return false;
    }


#pragma mark - CALLBACKS:


    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const websocket::Headers &headers) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if (repl == _replClient) {
            Assert(!_gotResponse);
            _gotResponse = true;
            Assert(status == 200);
            Assert(headers["Set-Cookie"_sl] == "flavor=chocolate-chip"_sl);
        }
    }

    virtual void replicatorGotTLSCertificate(slice certData) override {
    }

    virtual void replicatorStatusChanged(Replicator* repl,
                                         const Replicator::Status &status) override
    {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if (repl == _replClient) {
            Assert(_gotResponse);
            ++_statusChangedCalls;
            Log(">> Replicator is %-s, progress %lu/%lu, %lu docs",
                kC4ReplicatorActivityLevelNames[status.level],
                (unsigned long)status.progress.unitsCompleted,
                (unsigned long)status.progress.unitsTotal,
                (unsigned long)status.progress.documentCount);
            Assert(status.progress.unitsCompleted <= status.progress.unitsTotal);
            Assert(status.progress.documentCount < 1000000);
            if (status.progress.unitsTotal > 0) {
                Assert(status.progress.unitsCompleted >= _statusReceived.progress.unitsCompleted);
                Assert(status.progress.unitsTotal     >= _statusReceived.progress.unitsTotal);
                Assert(status.progress.documentCount  >= _statusReceived.progress.documentCount);
            }

            {
                _statusReceived = status;
                _checkStopWhenIdle();
            }
        }

        bool &finished = (repl == _replClient) ? _replicatorClientFinished : _replicatorServerFinished;
        Assert(!finished);
        if (status.level == kC4Stopped) {
            finished = true;
            if (_replicatorClientFinished && _replicatorServerFinished)
                _cond.notify_all();
        }
    }

    virtual void replicatorDocumentsEnded(Replicator *repl,
                                          const std::vector<Retained<ReplicatedRev>> &revs) override
    {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if (repl == _replClient) {
            Assert(!_replicatorClientFinished);
            for (auto &rev : revs) {
                auto dir = rev->dir();
                if (rev->error.code) {
                    if (dir == Dir::kPulling && rev->error.domain == LiteCoreDomain
                                             && rev->error.code == kC4ErrorConflict
                                             && _conflictHandler) {
                        Log(">> Replicator pull conflict for '%.*s'", SPLAT(rev->docID));
                        _conflictHandler(rev);
                    } else {
                        Log(">> Replicator %serror %s '%.*s' #%.*s: %s",
                            (rev->errorIsTransient ? "transient " : ""),
                            (dir == Dir::kPushing ? "pushing" : "pulling"),
                            SPLAT(rev->docID), SPLAT(rev->revID), rev->error.description().c_str());
                        if (!rev->errorIsTransient || !_ignoreTransientErrors) {
                            if (dir == Dir::kPushing)
                                _docPushErrors.emplace(rev->docID);
                            else
                                _docPullErrors.emplace(rev->docID);
                        }
                    }
                } else {
                    Log(">> Replicator %s '%.*s' #%.*s",
                        (dir == Dir::kPushing ? "pushed" : "pulled"), SPLAT(rev->docID), SPLAT(rev->revID));
                    _docsFinished.emplace(rev->docID);
                }
            }
        }
    }

    virtual void replicatorBlobProgress(Replicator *repl,
                                        const Replicator::BlobProgress &p) override
    {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if (p.dir == Dir::kPushing) {
            ++_blobPushProgressCallbacks;
            _lastBlobPushProgress = p;
        } else {
            ++_blobPullProgressCallbacks;
            _lastBlobPullProgress = p;
        }
        alloc_slice keyString(c4blob_keyToString(p.key));
        Log(">> Replicator %s blob '%.*s'%.*s [%.*s] (%" PRIu64 " / %" PRIu64 ")",
            (p.dir == Dir::kPushing ? "pushing" : "pulling"), SPLAT(p.docID),
            SPLAT(p.docProperty), SPLAT(keyString),
            p.bytesCompleted, p.bytesTotal);
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        std::unique_lock<std::mutex> lock(_mutex);

        if (repl == _replClient) {
            Log(">> Replicator closed with code=%d/%d, message=%.*s",
                status.reason, status.code, SPLAT(status.message));
        }
    }


#pragma mark - CONFLICT HANDLING


    // Installs a simple conflict handler equivalent to the one in CBL 2.0-2.5: it just picks one
    // side and tosses the other one out.
    void installConflictHandler() {
        c4::ref<C4Database> resolvDB = c4db_openAgain(db, nullptr);
        REQUIRE(resolvDB);
        auto& conflictHandlerRunning = _conflictHandlerRunning;
        _conflictHandler = [resolvDB, &conflictHandlerRunning](ReplicatedRev *rev) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            Log("Resolving conflict in '%.*s' ...", SPLAT(rev->docID));

            conflictHandlerRunning = true;
            TransactionHelper t(resolvDB);
            C4Error error;
            // Get the local rev:
            c4::ref<C4Document> doc = c4db_getDoc(resolvDB, rev->docID, true, kDocGetAll, &error);
            if (!doc) {
                WarnError("conflictHandler: Couldn't read doc '%.*s'", SPLAT(rev->docID));
                Assert(false, "conflictHandler: Couldn't read doc");
            }
            alloc_slice localRevID = doc->selectedRev.revID;
            C4RevisionFlags localFlags = doc->selectedRev.flags;
            FLDict localBody = c4doc_getProperties(doc);
            // Get the remote rev:
            if (!c4doc_selectNextLeafRevision(doc, true, false, &error)) {
                WarnError("conflictHandler: Couldn't get conflicting revision of '%.*s'", SPLAT(rev->docID));
                Assert(false, "conflictHandler: Couldn't get conflicting revision");
            }
            alloc_slice remoteRevID = doc->selectedRev.revID;
            C4RevisionFlags remoteFlags = doc->selectedRev.flags;
            if (c4doc_selectNextLeafRevision(doc, true, false, &error)) {   // no 3rd branch!
                WarnError("conflictHandler: Unexpected 3rd leaf revision in '%.*s'", SPLAT(rev->docID));
                Assert(false, "conflictHandler: Unexpected 3rd leaf revision");
            }

            bool remoteWins = false;

            // Figure out which branch should win:
            if ((localFlags & kRevDeleted) != (remoteFlags & kRevDeleted))
                remoteWins = (localFlags & kRevDeleted) == 0;       // deletion wins conflict
            else if (c4rev_getGeneration(localRevID) != c4rev_getGeneration(remoteRevID))
                remoteWins = c4rev_getGeneration(localRevID) < c4rev_getGeneration(remoteRevID);

            Log("Resolving conflict in '%.*s': local=#%.*s (%02X), remote=#%.*s (%02X); %s wins",
                SPLAT(rev->docID), SPLAT(localRevID), localFlags, SPLAT(remoteRevID), remoteFlags,
                (remoteWins ? "remote" : "local"));

            // Resolve. The remote rev has to win, in that it has to stay on the main branch, to avoid
            // conflicts with the server. But if we want the local copy to really win, we use its body:
            FLDict mergedBody = nullptr;
            C4RevisionFlags mergedFlags = remoteFlags;
            if (remoteWins) {
                mergedBody = localBody;
                mergedFlags = localFlags;
            }
            if (!c4doc_resolveConflict2(doc, remoteRevID, localRevID,
                                        mergedBody, mergedFlags, &error)) {
                WarnError("conflictHandler: c4doc_resolveConflict failed in '%.*s'", SPLAT(rev->docID));
                Assert(false, "conflictHandler: c4doc_resolveConflict failed");
            }
            Assert((doc->flags & kDocConflicted) == 0);
            if (!c4doc_save(doc, 0, &error)) {
                WarnError("conflictHandler: c4doc_save failed in '%.*s'", SPLAT(rev->docID));
                Assert(false, "conflictHandler: c4doc_save failed");
            }
            conflictHandlerRunning = false;
        };
    }


#pragma mark - ADDING DOCS/REVISIONS:


    // Pause the current thread for an interval. If the interval is negative, it will randomize.
    static void sleepFor(duration interval) {
        auto ticks = interval.count();
        if (ticks < 0) {
            ticks = RandomNumber(uint32_t(-ticks)) + RandomNumber(uint32_t(-ticks));
            interval = duration(ticks);
        }
        std::this_thread::sleep_for(interval);
    }

    int addDocs(C4Database *db, duration interval, int total) {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        C4Collection* coll = c4db_getDefaultCollection(db, nullptr);
        Assert(coll != nullptr);
        return addDocs(coll, interval, total, "newdoc");
    }

    void addRevs(C4Database *db, duration interval,
                 alloc_slice docID,
                 int firstRev, int totalRevs, bool useFakeRevIDs)
    {
        C4Collection* coll = c4db_getDefaultCollection(db, nullptr);
        Assert(coll != nullptr);
        addRevs(coll, interval, docID, firstRev, totalRevs, useFakeRevIDs);
    }
    
    int addDocs(C4Collection* coll, duration interval, int total, string idPrefix) {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        int docNo = 1;
        for (int i = 1; docNo <= total; i++) {
            Log("-------- Creating %d docs --------", 2*i);
            TransactionHelper t(db);
            for (int j = 0; j < 2*i; j++) {
                char docID[20];
                sprintf(docID, "%s%d", idPrefix.c_str(), docNo++);
                createRev(coll, c4str(docID), (isRevTrees() ? "1-11"_sl : "1@*"_sl), kFleeceBody);
            }
        }
        Log("-------- Done creating docs --------");
        return docNo - 1;
    }

    void addRevs(C4Collection *coll, duration interval,
                 alloc_slice docID,
                 int firstRev, int totalRevs, bool useFakeRevIDs)
    {
        auto db = c4coll_getDatabase(coll);
        auto name = c4db_getName(db);
        alloc_slice collPath = Options::collectionSpecToPath(c4coll_getSpec(coll));
        for (int i = 0; i < totalRevs; i++) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            int revNo = firstRev + i;
            sleepFor(interval);
            TransactionHelper t(db);
            string revID;
            if (useFakeRevIDs) {
                revID = isRevTrees() ? format("%d-ffff", revNo) : format("%d@*", revNo);
                createRev(coll, docID, slice(revID), alloc_slice(kFleeceBody));
            } else {
                string json = format("{\"db\":\"%p\",\"i\":%d}", db, revNo);
                revID = createFleeceRev(db, docID, nullslice, slice(json));
            }
            Log("-------- %.*s/%.*s %d: Created rev '%.*s' #%s --------",
                SPLAT(name), SPLAT(collPath), revNo, SPLAT(docID), revID.c_str());
        }
        Log("-------- %.*s/%.*s: Done creating revs --------", SPLAT(name), SPLAT(collPath));
    }

    static std::thread* runInParallel(std::function<void()> callback) {
        return new std::thread([=]() mutable {
            callback();
        });
    }

    void addDocsInParallel(duration interval, int total) {
        _parallelThread.reset(runInParallel([=]() {
            _expectedDocumentCount = addDocs(db, interval, total);
            sleepFor(1s); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }

    void addRevsInParallel(duration interval, alloc_slice docID, int firstRev, int totalRevs,
                           bool useFakeRevIDs = true) {
        _parallelThread.reset( runInParallel([=]() {
            addRevs(db, interval, docID, firstRev, totalRevs, useFakeRevIDs);
            sleepFor(1s); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }


#pragma mark - VALIDATION:


    alloc_slice absoluteRevID(C4Document *doc) {
        if (isRevTrees())
            return alloc_slice(doc->revID);
        else
            return alloc_slice(c4doc_getRevisionHistory(doc, 999, nullptr, 0));
    }


#define fastREQUIRE(EXPR)  if (EXPR) ; else REQUIRE(EXPR)       // REQUIRE() is kind of expensive

    void compareDocs(C4Document *doc1, C4Document *doc2) {
        const auto kPublicDocumentFlags = (kDocDeleted | kDocConflicted | kDocHasAttachments);

        fastREQUIRE(doc1->docID == doc2->docID);
        fastREQUIRE(absoluteRevID(doc1) == absoluteRevID(doc2));
        fastREQUIRE((doc1->flags & kPublicDocumentFlags) == (doc2->flags & kPublicDocumentFlags));

        // Compare canonical JSON forms of both docs:
        Dict rev1 = c4doc_getProperties(doc1), rev2 = c4doc_getProperties(doc2);
        if (!rev1.isEqual(rev2)) {        // fast check to avoid expensive toJSON
            alloc_slice json1 = rev1.toJSON(true, true);
            alloc_slice json2 = rev2.toJSON(true, true);
            CHECK(json1 == json2);
        }
    }

    void compareDatabases(bool db2MayHaveMoreDocs =false, bool compareDeletedDocs =true) {
        C4Log(">> Comparing databases...");
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (compareDeletedDocs)
            options.flags |= kC4IncludeDeleted;
        C4Error error;
        c4::ref<C4DocEnumerator> e1 = c4db_enumerateAllDocs(db, &options, ERROR_INFO(error));
        REQUIRE(e1);
        c4::ref<C4DocEnumerator> e2 = c4db_enumerateAllDocs(db2, &options, ERROR_INFO(error));
        REQUIRE(e2);

        unsigned i = 0;
        while (c4enum_next(e1, ERROR_INFO(error))) {
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
        if (!db2MayHaveMoreDocs) {
            REQUIRE(!c4enum_next(e2, ERROR_INFO(error)));
            REQUIRE(error.code == 0);
        }
    }

    void validateCheckpoint(C4Database *database, bool local,
                            const char *body, const char *meta = "1-") {
        validateCollectionCheckpoint(database, 0, local, body, meta);
    }

    void validateCheckpoints(C4Database *localDB, C4Database *remoteDB,
                             const char *body, const char *meta = "1-cc") {
        validateCollectionCheckpoints(localDB, remoteDB, 0, body, meta);
    }
    
    void clearCheckpoint(C4Database *database, bool local) {
        clearCollectionCheckpoint(database, 0, local);
    }

    void validateCollectionCheckpoint(C4Database *database, unsigned collectionIndex, bool local,
                                      const char *body, const char *meta = "1-") {
        C4Error err = {};
        C4Slice storeName;
        if(local) {
            storeName = C4STR("checkpoints");
        } else {
            storeName = C4STR("peerCheckpoints");
        }

        REQUIRE(collectionIndex < _checkpointIDs.size());
        alloc_slice checkpointID = _checkpointIDs[collectionIndex];
        c4::ref<C4RawDocument> doc( c4raw_get(database,
                                              storeName,
                                              checkpointID,
                                              WITH_ERROR(&err)) );
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << string(checkpointID));
        REQUIRE(doc);
        CHECK(doc->body == c4str(body));
        if (!local)
            CHECK(c4rev_getGeneration(doc->meta) >= c4rev_getGeneration(c4str(meta)));
    }
    
    void validateCollectionCheckpoints(C4Database *localDB, C4Database *remoteDB, unsigned collectionIndex,
                                       const char *body, const char *meta = "1-cc") {
        validateCollectionCheckpoint(localDB, collectionIndex, true,  body, meta);
        validateCollectionCheckpoint(remoteDB, collectionIndex, false, body, meta);
    }
    
    void clearCollectionCheckpoint(C4Database *database, unsigned collectionIndex, bool local) {
        C4Error err;
        C4Slice storeName;
        if(local) {
            storeName = C4STR("checkpoints");
        } else {
            storeName = C4STR("peerCheckpoints");
        }
        
        REQUIRE( c4raw_put(database,
                           storeName,
                           _checkpointIDs[collectionIndex],
                           kC4SliceNull, kC4SliceNull, ERROR_INFO(&err)) );
    }

    template <class SET>
    static std::vector<std::string> asVector(const SET &strings) {
        std::vector<std::string> out;
        for (const std::string &s : strings)
            out.push_back(s);
        return out;
    }

    C4Database* db2 {nullptr};
    Retained<Replicator> _replClient, _replServer;
    std::vector<alloc_slice> _checkpointIDs;
    std::unique_ptr<std::thread> _parallelThread;
    bool _stopOnIdle {0};
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _replicatorClientFinished {false}, _replicatorServerFinished {false};
    C4ReplicatorProgressLevel _clientProgressLevel {}, _serverProgressLevel {};
    bool _gotResponse {false};
    Replicator::Status _statusReceived { };
    unsigned _statusChangedCalls {0};
    int64_t _expectedDocumentCount {0};
    int64_t _expectedUnitsComplete {-1};
    C4Error _expectedError {};
    std::set<std::string> _docPushErrors, _docPullErrors;
    std::set<std::string> _expectedDocPushErrors, _expectedDocPullErrors;
    bool _ignoreLackOfDocErrors = false;
    bool _ignoreTransientErrors = false;
    bool _checkDocsFinished {true};
    std::multiset<std::string> _docsFinished, _expectedDocsFinished;
    unsigned _blobPushProgressCallbacks {0}, _blobPullProgressCallbacks {0};
    Replicator::BlobProgress _lastBlobPushProgress {}, _lastBlobPullProgress {};
    std::function<void(ReplicatedRev*)> _conflictHandler;
    bool _conflictHandlerRunning {false};
};

