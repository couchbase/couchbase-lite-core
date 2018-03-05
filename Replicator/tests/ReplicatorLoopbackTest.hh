//
//  ReplicatorLoopbackTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "StringUtil.hh"
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
    static constexpr duration kLatency              = chrono::milliseconds(5);


    ReplicatorLoopbackTest()
    :C4Test(0)
    ,_provider(kLatency)
    ,db2(createDatabase("2"))
    { }

    ~ReplicatorLoopbackTest() {
        if (_parallelThread)
            _parallelThread->join();
        _replClient = _replServer = nullptr;
        CHECK(WebSocket::gInstanceCount == 0);
        C4Error error;
        c4db_delete(db2, &error);
        c4db_free(db2);
    }

    // opts1 is the options for _db; opts2 is the options for _db2
    void runReplicators(Replicator::Options opts1, Replicator::Options opts2) {
        _replicatorFinished = false;
        _gotResponse = false;
        _statusChangedCalls = 0;
        _statusReceived = {};

        C4Database *dbClient = db, *dbServer = db2;
        if (opts2.push > kC4Passive || opts2.pull > kC4Passive) {
            // always make opts1 the active (client) side
            swap(dbServer, dbClient);
            swap(opts1, opts2);
        }

        // Create client (active) and server (passive) replicators:
        Address clientAddress{"ws", "cli"}, serverAddress{"ws", "srv"};
        _replClient = new Replicator(dbClient, _provider, serverAddress, *this, opts1);
        _replServer = new Replicator(dbServer, _provider.createWebSocket(clientAddress), *this, opts2);

        // Response headers:
        Encoder enc;
        enc.beginDict();
        enc.writeKey("Set-Cookie"_sl);
        enc.writeString("flavor=chocolate-chip");
        enc.endDict();
        AllocedDict headers(enc.finish());

        // Bind the replicators' WebSockets and start them:
        _provider.bind(_replClient->webSocket(), _replServer->webSocket(), headers);
        _replClient->start();
        _replServer->start();

        Log("Waiting for replication to complete...");
        while (!_replicatorFinished)
            this_thread::sleep_for(chrono::milliseconds(100));
        
        Log(">>> Replication complete <<<");
        _checkpointID = _replClient->checkpointID();
        CHECK(_gotResponse);
        CHECK(_statusChangedCalls > 0);
        CHECK(_statusReceived.level == kC4Stopped);
        CHECK(_statusReceived.progress.unitsCompleted == _statusReceived.progress.unitsTotal);
        if (_expectedDocumentCount >= 0)
            CHECK(_statusReceived.progress.documentCount == uint64_t(_expectedDocumentCount));
        CHECK(_statusReceived.error.code == _expectedError.code);
        if (_expectedError.code)
            CHECK(_statusReceived.error.domain == _expectedError.domain);
        CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
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

    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const AllocedDict &headers) override {
        if (repl == _replClient) {
            CHECK(!_gotResponse);
            _gotResponse = true;
            CHECK(status == 200);
            CHECK(headers["Set-Cookie"].asString() == "flavor=chocolate-chip"_sl);
        }
    }

    virtual void replicatorStatusChanged(Replicator* repl,
                                         const Replicator::Status &status) override
    {
        // Note: Can't use Catch on a background thread
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
            _statusReceived = status;

            if (_stopOnIdle && status.level == kC4Idle && (_expectedDocumentCount <= 0 || status.progress.documentCount == _expectedDocumentCount)) {
                Log(">>    Stopping idle replicator...");
                repl->stop();
            }
        }

        if (_replClient->status().level == kC4Stopped && _replServer->status().level == kC4Stopped)
            _replicatorFinished = true;
    }

    virtual void replicatorDocumentError(Replicator *repl,
                                         bool pushing,
                                         slice docID,
                                         C4Error error,
                                         bool transient) override
    {
        char message[256];
        c4error_getMessageC(error, message, sizeof(message));
        Log(">> Replicator %serror %s '%.*s': %s",
            (transient ? "transient " : ""),
            (pushing ? "pushing" : "pulling"),
            SPLAT(docID), message);
        if (pushing)
            _docPushErrors.emplace(docID);
        else
            _docPullErrors.emplace(docID);
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        if (repl == _replClient) {
            Log(">> Replicator closed with code=%d/%d, message=%.*s",
                status.reason, status.code, SPLAT(status.message));
        }
    }


    void runInParallel(function<void(C4Database*)> callback) {
        C4Error error;
        C4Database *parallelDB = c4db_openAgain(db, &error);
        REQUIRE(parallelDB != nullptr);

        _parallelThread.reset(new thread([=]() mutable {
            callback(parallelDB);
            c4db_free(parallelDB);
        }));
    }

    void addDocsInParallel(duration interval, int total) {
        runInParallel([=](C4Database *bgdb) {
            int docNo = 1;
            for (int i = 1; docNo <= total; i++) {
                this_thread::sleep_for(interval);
                Log("-------- Creating %d docs --------", 2*i);
                c4::Transaction t(bgdb);
                C4Error err;
                Assert(t.begin(&err));
                for (int j = 0; j < 2*i; j++) {
                    char docID[20];
                    sprintf(docID, "newdoc%d", docNo++);
                    createRev(bgdb, c4str(docID), "1-11"_sl, kFleeceBody);
                }
                Assert(t.commit(&err));
            }
            Log("-------- Done creating docs --------");
            _expectedDocumentCount = docNo - 1;
            _stopOnIdle = true;
        });
    }

    void addRevsInParallel(duration interval, alloc_slice docID, int firstRev, int totalRevs) {
        runInParallel([=](C4Database *bgdb) {
            for (int i = 0; i < totalRevs; i++) {
                int revNo = firstRev + i;
                this_thread::sleep_for(interval);
                Log("-------- Creating rev %.*s # %d --------", SPLAT(docID), revNo);
                c4::Transaction t(bgdb);
                C4Error err;
                Assert(t.begin(&err));
                char revID[20];
                sprintf(revID, "%d-ffff", revNo);
                createRev(bgdb, docID, c4str(revID), kFleeceBody);
                Assert(t.commit(&err));
            }
            Log("-------- Done creating revs --------");
            _stopOnIdle = true;
        });
    }

    void compareDocs(C4Document *doc1, C4Document *doc2) {
        const auto kPublicDocumentFlags = (kDocDeleted | kDocConflicted | kDocHasAttachments);

        REQUIRE(doc1->docID == doc2->docID);
        REQUIRE(doc1->revID == doc2->revID);
        REQUIRE((doc1->flags & kPublicDocumentFlags) == (doc2->flags & kPublicDocumentFlags));

        // Compare canonical JSON forms of both docs:
        Value root1 = Value::fromData(doc1->selectedRev.body);
        Value root2 = Value::fromData(doc2->selectedRev.body);
        alloc_slice json1 = root1.toJSON(c4db_getFLSharedKeys(db), true, true);
        alloc_slice json2 = root2.toJSON(c4db_getFLSharedKeys(db2), true, true);
        CHECK(json1 == json2);
    }

    void compareDatabases(bool db2MayHaveMoreDocs =false, bool compareDeletedDocs =true) {
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
            REQUIRE(doc1);
            INFO("db document #" << i << ": '" << asstring(doc1->docID) << "'");
            REQUIRE(c4enum_next(e2, &error));
            c4::ref<C4Document> doc2 = c4enum_getDocument(e2, &error);
            REQUIRE(doc2);
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
        c4::ref<C4RawDocument> doc( c4raw_get(database,
                                              (local ? C4STR("checkpoints") : C4STR("peerCheckpoints")),
                                              _checkpointID,
                                              &err) );
        INFO("Checking " << (local ? "local" : "remote") << " checkpoint '" << asstring(_checkpointID) << "'; err = " << err.domain << "," << err.code);
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
        REQUIRE( c4raw_put(database,
                           (local ? C4STR("checkpoints") : C4STR("peerCheckpoints")),
                           _checkpointID,
                           kC4SliceNull, kC4SliceNull, &err) );
    }

    static vector<string> asVector(const set<string> strings) {
        vector<string> out;
        for (const string &s : strings)
            out.push_back(s);
        return out;
    }

    LoopbackProvider _provider;
    C4Database* db2 {nullptr};
    Retained<Replicator> _replClient, _replServer;
    alloc_slice _checkpointID;
    unique_ptr<thread> _parallelThread;
    atomic<bool> _stopOnIdle {0};
    atomic<bool> _replicatorFinished {0};
    bool _gotResponse {false};
    Replicator::Status _statusReceived { };
    unsigned _statusChangedCalls {0};
    int64_t _expectedDocumentCount {0};
    C4Error _expectedError {};
    set<string> _docPushErrors, _docPullErrors;
    set<string> _expectedDocPushErrors, _expectedDocPullErrors;
};

