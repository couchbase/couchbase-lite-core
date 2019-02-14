//
//  ReplicatorLoopbackTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "ReplicatorTuning.hh"
#include "StringUtil.hh"
#include "SecureRandomize.hh"
#include <algorithm>
#include <chrono>
#include <thread>

#include "c4Test.hh"


using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


class ReplicatorLoopbackTest : public C4Test, Replicator::Delegate {
public:
    static constexpr duration kLatency              = chrono::milliseconds(50);


    ReplicatorLoopbackTest()
    :C4Test(0)
    ,db2(createDatabase("2"))
    {
        // Change tuning param so that tests will actually create deltas, despite using small
        // document bodies:
        litecore::repl::tuning::kMinBodySizeForDelta = 0;
    }

    ~ReplicatorLoopbackTest() {
        if (_parallelThread)
            _parallelThread->join();
        _replClient = _replServer = nullptr;
        C4Error error;
        REQUIRE(c4db_delete(db2, &error));
        c4db_free(db2);
    }

    // opts1 is the options for _db; opts2 is the options for _db2
    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        _gotResponse = false;
        _statusChangedCalls = 0;
        _statusReceived = {};
        _replicatorClientFinished = _replicatorServerFinished = false;

        c4::ref<C4Database> dbClient = c4db_openAgain(db, nullptr);
        c4::ref<C4Database> dbServer = c4db_openAgain(db2, nullptr);
        REQUIRE(dbClient);
        REQUIRE(dbServer);
        if (opts2.push > kC4Passive || opts2.pull > kC4Passive) {
            // always make opts1 the active (client) side
            swap(dbServer, dbClient);
            swap(opts1, opts2);
        }

        // Create client (active) and server (passive) replicators:
        _replClient = new Replicator(dbClient,
                                     new LoopbackWebSocket(alloc_slice("ws://srv/"_sl), Role::Client, kLatency),
                                     *this, opts1);
        _replServer = new Replicator(dbServer,
                                     new LoopbackWebSocket(alloc_slice("ws://cli/"_sl), Role::Server, kLatency),
                                     *this, opts2);

        // Response headers:
        Encoder enc;
        enc.beginDict();
        enc.writeKey("Set-Cookie"_sl);
        enc.writeString("flavor=chocolate-chip");
        enc.endDict();
        AllocedDict headers(enc.finish());

        // Bind the replicators' WebSockets and start them:
        LoopbackWebSocket::bind(_replClient->webSocket(), _replServer->webSocket(), headers);
        Stopwatch st;
        _replClient->start();
        _replServer->start();

        {
            Log("Waiting for replication to complete...");
            unique_lock<mutex> lock(_mutex);
            while (!_replicatorClientFinished || !_replicatorServerFinished)
                _cond.wait(lock);
        }
        
        Log(">>> Replication complete (%.3f sec) <<<", st.elapsed());
        _checkpointID = _replClient->checkpointID();
        _replClient = _replServer = nullptr;

        CHECK(_gotResponse);
        CHECK(_statusChangedCalls > 0);
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if(_expectedUnitsComplete >= 0)
            CHECK(_expectedUnitsComplete == _statusReceived.progress.unitsCompleted);
        if (_expectedDocumentCount >= 0)
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
        CHECK(_statusReceived.error.code == _expectedError.code);
        if (_expectedError.code)
            CHECK(_statusReceived.error.domain == _expectedError.domain);
        CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
        if (_checkDocsFinished)
            CHECK(asVector(_docsFinished) == asVector(_expectedDocsFinished));
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
        lock_guard<mutex> lock(_mutex);
        if (!_stopOnIdle) {
            _stopOnIdle = true;
            if (!_checkStopWhenIdle())
                Log(">>    Will stop replicator when idle...");
        }
    }

    // must be holding _mutex to call this
    bool _checkStopWhenIdle() {
        if (_stopOnIdle && _statusReceived.level == kC4Idle) {
            Log(">>    Stopping idle replicator...");
            _replClient->stop();
            return true;
        }
        return false;
    }


#pragma mark - CALLBACKS:


    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const AllocedDict &headers) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        if (repl == _replClient) {
            Assert(!_gotResponse);
            _gotResponse = true;
            Assert(status == 200);
            Assert(headers["Set-Cookie"].asString() == "flavor=chocolate-chip"_sl);
        }
    }

    virtual void replicatorStatusChanged(Replicator* repl,
                                         const Replicator::Status &status) override
    {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
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
                lock_guard<mutex> lock(_mutex);
                _statusReceived = status;
                _checkStopWhenIdle();
            }
        }

        if (status.level == kC4Stopped) {
            unique_lock<mutex> lock(_mutex);
            if (repl == _replClient)
                _replicatorClientFinished = true;
            else
                _replicatorServerFinished = true;
            if (_replicatorClientFinished && _replicatorServerFinished)
                _cond.notify_all();
        }
    }

    virtual void replicatorDocumentsEnded(Replicator *repl,
                                          const vector<Retained<ReplicatedRev>> &revs) override
    {
        if (repl == _replClient) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            for (auto &rev : revs) {
                auto dir = rev->dir();
                if (rev->error.code) {
                    if (dir == Dir::kPulling && rev->error.domain == LiteCoreDomain
                                             && rev->error.code == kC4ErrorConflict
                                             && _conflictHandler) {
                        Log(">> Replicator pull conflict for '%.*s'", SPLAT(rev->docID));
                        _conflictHandler(rev);
                    } else {
                        char message[256];
                        c4error_getDescriptionC(rev->error, message, sizeof(message));
                        Log(">> Replicator %serror %s '%.*s' #%.*s: %s",
                            (rev->errorIsTransient ? "transient " : ""),
                            (dir == Dir::kPushing ? "pushing" : "pulling"),
                            SPLAT(rev->docID), SPLAT(rev->revID), message);
                        if (dir == Dir::kPushing)
                            _docPushErrors.emplace(rev->docID);
                        else
                            _docPullErrors.emplace(rev->docID);
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
        if (p.dir == Dir::kPushing) {
            ++_blobPushProgressCallbacks;
            _lastBlobPushProgress = p;
        } else {
            ++_blobPullProgressCallbacks;
            _lastBlobPullProgress = p;
        }
        alloc_slice keyString(c4blob_keyToString(p.key));
        Log(">> Replicator %s blob '%.*s'%.*s [%.*s] (%llu / %llu)",
            (p.dir == Dir::kPushing ? "pushing" : "pulling"), SPLAT(p.docID),
            SPLAT(p.docProperty), SPLAT(keyString),
            p.bytesCompleted, p.bytesTotal);
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
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
        _conflictHandler = [resolvDB](ReplicatedRev *rev) {
            // Careful: This is called on a background thread!
            TransactionHelper t(resolvDB);
            C4Error error;
            // Get the local rev:
            c4::ref<C4Document> doc = c4doc_get(resolvDB, rev->docID, true, &error);
            REQUIRE(doc);
            alloc_slice localRevID = doc->selectedRev.revID;
            C4RevisionFlags localFlags = doc->selectedRev.flags;
            slice localBody = doc->selectedRev.body;
            // Get the remote rev:
            REQUIRE(c4doc_selectNextLeafRevision(doc, true, false, &error));
            alloc_slice remoteRevID = doc->selectedRev.revID;
            C4RevisionFlags remoteFlags = doc->selectedRev.flags;
            REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, &error));   // no 3rd branch!

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
            slice mergedBody;
            C4RevisionFlags mergedFlags = remoteFlags;
            if (remoteWins) {
                mergedBody = localBody;
                mergedFlags = localFlags;
            }
            CHECK(c4doc_resolveConflict(doc, remoteRevID, localRevID,
                                        mergedBody, mergedFlags, &error));
            CHECK(c4doc_save(doc, 0, &error));
        };
    }


#pragma mark - ADDING DOCS/REVISIONS:


    // Pause the current thread for an interval. If the interval is negative, it will randomize.
    static void sleepFor(duration interval) {
        long ticks = interval.count();
        if (ticks < 0) {
            ticks = RandomNumber(uint32_t(-ticks)) + RandomNumber(uint32_t(-ticks));
            interval = duration(ticks);
        }
        this_thread::sleep_for(interval);
    }

    static int addDocs(C4Database *db, duration interval, int total) {
        // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
        int docNo = 1;
        for (int i = 1; docNo <= total; i++) {
            sleepFor(interval);
            Log("-------- Creating %d docs --------", 2*i);
            c4::Transaction t(db);
            C4Error err;
            Assert(t.begin(&err));
            for (int j = 0; j < 2*i; j++) {
                char docID[20];
                sprintf(docID, "newdoc%d", docNo++);
                createRev(db, c4str(docID), "1-11"_sl, kFleeceBody);
            }
            Assert(t.commit(&err));
        }
        Log("-------- Done creating docs --------");
        return docNo - 1;
    }

    void addRevs(C4Database *db, duration interval,
                 alloc_slice docID,
                 int firstRev, int totalRevs, bool useFakeRevIDs)
    {
        const char* name = (db == this->db) ? "db" : "db2";
        for (int i = 0; i < totalRevs; i++) {
            // Note: Can't use Catch (CHECK, REQUIRE) on a background thread
            int revNo = firstRev + i;
            sleepFor(interval);
            c4::Transaction t(db);
            C4Error err;
            Assert(t.begin(&err));
            string revID;
            if (useFakeRevIDs) {
                revID = format("%d-ffff", revNo);
                createRev(db, docID, slice(revID), alloc_slice(kFleeceBody));
            } else {
                string json = format("{\"db\":\"%p\",\"i\":%d}", db, revNo);
                revID = createFleeceRev(db, docID, nullslice, slice(json));
            }
            Log("-------- %s %d: Created rev '%.*s' #%s --------", name, revNo, SPLAT(docID), revID.c_str());
            Assert(t.commit(&err));
        }
        Log("-------- %s: Done creating revs --------", name);
    }

    static thread* runInParallel(function<void()> callback) {
        C4Error error;
        return new thread([=]() mutable {
            callback();
        });
    }

    void addDocsInParallel(duration interval, int total) {
        _parallelThread.reset(runInParallel([=]() {
            _expectedDocumentCount = addDocs(db, interval, total);
            sleepFor(chrono::seconds(1)); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }

    void addRevsInParallel(duration interval, alloc_slice docID, int firstRev, int totalRevs,
                           bool useFakeRevIDs = true) {
        _parallelThread.reset( runInParallel([=]() {
            addRevs(db, interval, docID, firstRev, totalRevs, useFakeRevIDs);
            sleepFor(chrono::seconds(1)); // give replicator a moment to detect the latest revs
            stopWhenIdle();
        }));
    }


#pragma mark - VALIDATION:


#define fastREQUIRE(EXPR)  if (EXPR) ; else REQUIRE(EXPR)       // REQUIRE() is kind of expensive

    void compareDocs(C4Document *doc1, C4Document *doc2) {
        const auto kPublicDocumentFlags = (kDocDeleted | kDocConflicted | kDocHasAttachments);

        fastREQUIRE(doc1->docID == doc2->docID);
        fastREQUIRE(doc1->revID == doc2->revID);
        fastREQUIRE((doc1->flags & kPublicDocumentFlags) == (doc2->flags & kPublicDocumentFlags));

        // Compare canonical JSON forms of both docs:
        Doc rev1 = c4::getFleeceDoc(doc1), rev2 = c4::getFleeceDoc(doc2);
        if (!rev1.root().isEqual(rev2.root())) {        // fast check to avoid expensive toJSON
            alloc_slice json1 = rev1.root().toJSON(true, true);
            alloc_slice json2 = rev2.root().toJSON(true, true);
            CHECK(json1 == json2);
        }
    }

    void compareDatabases(bool db2MayHaveMoreDocs =false, bool compareDeletedDocs =true) {
        C4Log(">> Comparing databases...");
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (compareDeletedDocs)
            options.flags |= kC4IncludeDeleted;
        C4Error error;
        c4::ref<C4DocEnumerator> e1 = c4db_enumerateAllDocs(db, &options, &error);
        REQUIRE(e1);
        c4::ref<C4DocEnumerator> e2 = c4db_enumerateAllDocs(db2, &options, &error);
        REQUIRE(e2);

        unsigned i = 0;
        while (c4enum_next(e1, &error)) {
            c4::ref<C4Document> doc1 = c4enum_getDocument(e1, &error);
            fastREQUIRE(doc1);
            INFO("db document #" << i << ": '" << slice(doc1->docID).asString() << "'");
            bool ok = c4enum_next(e2, &error);
            fastREQUIRE(ok);
            c4::ref<C4Document> doc2 = c4enum_getDocument(e2, &error);
            fastREQUIRE(doc2);
            compareDocs(doc1, doc2);
            ++i;
        }
        REQUIRE(error.code == 0);
        if (!db2MayHaveMoreDocs) {
            REQUIRE(!c4enum_next(e2, &error));
            REQUIRE(error.code == 0);
        }
    }

    void validateCheckpoint(C4Database *database, bool local,
                            const char *body, const char *meta = "1-") {
        C4Error err;
		C4Slice storeName;
		if(local) {
			storeName = C4STR("checkpoints");
		} else {
			storeName = C4STR("peerCheckpoints");
		}

        c4::ref<C4RawDocument> doc( c4raw_get(database,
                                              storeName,
                                              _checkpointID,
                                              &err) );
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << string(_checkpointID) << "'; err = " << err.domain << "," << err.code);
        REQUIRE(doc);
        CHECK(doc->body == c4str(body));
        if (!local)
            CHECK(c4rev_getGeneration(doc->meta) >= c4rev_getGeneration(c4str(meta)));
    }

    void validateCheckpoints(C4Database *localDB, C4Database *remoteDB,
                             const char *body, const char *meta = "1-cc") {
        validateCheckpoint(localDB,  true,  body, meta);
        validateCheckpoint(remoteDB, false, body, meta);
    }

    void clearCheckpoint(C4Database *database, bool local) {
        C4Error err;
		C4Slice storeName;
		if(local) {
			storeName = C4STR("checkpoints");
		} else {
			storeName = C4STR("peerCheckpoints");
		}

        REQUIRE( c4raw_put(database,
                           storeName,
                           _checkpointID,
                           kC4SliceNull, kC4SliceNull, &err) );
    }

    template <class SET>
    static vector<string> asVector(const SET &strings) {
        vector<string> out;
        for (const string &s : strings)
            out.push_back(s);
        return out;
    }

    C4Database* db2 {nullptr};
    Retained<Replicator> _replClient, _replServer;
    alloc_slice _checkpointID;
    unique_ptr<thread> _parallelThread;
    bool _stopOnIdle {0};
    mutex _mutex;
    condition_variable _cond;
    bool _replicatorClientFinished {false}, _replicatorServerFinished {false};
    bool _gotResponse {false};
    Replicator::Status _statusReceived { };
    unsigned _statusChangedCalls {0};
    int64_t _expectedDocumentCount {0};
    int64_t _expectedUnitsComplete {-1};
    C4Error _expectedError {};
    set<string> _docPushErrors, _docPullErrors;
    set<string> _expectedDocPushErrors, _expectedDocPullErrors;
    bool _checkDocsFinished {true};
    multiset<string> _docsFinished, _expectedDocsFinished;
    unsigned _blobPushProgressCallbacks {0}, _blobPullProgressCallbacks {0};
    Replicator::BlobProgress _lastBlobPushProgress {}, _lastBlobPullProgress {};
    function<void(ReplicatedRev*)> _conflictHandler;
};

