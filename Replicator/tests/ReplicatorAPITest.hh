//
//  ReplicatorAPITest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4LibWebSocketFactory.h"
#include "Response.hh"
#include "make_unique.h"
#include <iostream>
#include "c4Test.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;


class ReplicatorAPITest : public C4Test {
public:
    // Default address to replicate with (individual tests can override this):
    constexpr static const C4Address kDefaultAddress {kC4Replicator2Scheme,
                                                      C4STR("localhost"),
                                                      4984};
    // Common database names:
    constexpr static const C4String kScratchDBName = C4STR("scratch");
    constexpr static const C4String kITunesDBName = C4STR("itunes");
    constexpr static const C4String kWikipedia1kDBName = C4STR("wikipedia1k");
    constexpr static const C4String kProtectedDBName = C4STR("seekrit");
    constexpr static const C4String kImagesDBName = C4STR("images");

    ReplicatorAPITest()
    :C4Test(0)
    {
        RegisterC4LWSWebSocketFactory();
        // Environment variables can also override the default address above:
        const char *hostname = getenv("REMOTE_HOST");
        if (hostname)
            _address.hostname = c4str(hostname);
        const char *portStr = getenv("REMOTE_PORT");
        if (portStr)
            _address.port = (uint16_t)strtol(portStr, nullptr, 10);
        const char *remoteDB = getenv("REMOTE_DB");
        if (remoteDB)
            _remoteDBName = c4str(remoteDB);
    }

    // Create an empty database db2 and make it the target of the replication
    void createDB2() {
        auto db2Path = TempDir() + "cbl_core_test2.cblite2";
        auto db2PathSlice = c4str(db2Path.c_str());

        auto config = c4db_getConfig(db);
        C4Error error;
        if (!c4db_deleteAtPath(db2PathSlice, &error))
            REQUIRE(error.code == 0);
        db2 = c4db_open(db2PathSlice, config, &error);
        REQUIRE(db2 != nullptr);

        _address = { };
        _remoteDBName = nullslice;
    }

    void enableDocProgressNotifications() {
        Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionProgressLevel));
        enc.writeInt(1);
        enc.endDict();
        _options = AllocedDict(enc.finish());
    }

    bool validate(slice docID, Dict body) {
        //TODO: Do something here
        return true;
    }

    static bool onValidate(FLString docID, C4RevisionFlags flags, FLDict body, void *context) {
        return ((ReplicatorAPITest*)context)->validate(docID, body);
    }

    void logState(C4ReplicatorStatus status) {
        if (status.error.code) {
            char message[200];
            c4error_getDescriptionC(status.error, message, sizeof(message));
            C4Log("*** C4Replicator state: %-s, progress=%llu/%llu, error=%s",
                  kC4ReplicatorActivityLevelNames[status.level],
                  status.progress.unitsCompleted, status.progress.unitsTotal,
                  message);
        } else {
            C4Log("*** C4Replicator state: %-s, progress=%llu/%llu",
                  kC4ReplicatorActivityLevelNames[status.level],
                  status.progress.unitsCompleted, status.progress.unitsTotal);
        }
    }

    void stateChanged(C4Replicator *r, C4ReplicatorStatus s) {
        lock_guard<mutex> lock(_mutex);

        Assert(r == _repl);      // can't call REQUIRE on a background thread
        logState(s);
        _callbackStatus = s;
        ++_numCallbacks;
        Assert(_numCallbacksWithLevel[(int)kC4Stopped] == 0);   // Stopped must be the final state
        _numCallbacksWithLevel[(int)s.level]++;
        if (s.level == kC4Busy)
            Assert(s.error.code == 0);                          // Busy state shouldn't have error

        if (!_headers) {
            _headers = AllocedDict(alloc_slice(c4repl_getResponseHeaders(_repl)));
            if (!!_headers) {
                for (Dict::iterator header(_headers); header; ++header)
                    C4Log("    %.*s: %.*s", SPLAT(header.keyString()), SPLAT(header.value().asString()));
            }
        }

        if (!_socketFactory && !db2) {  // i.e. this is a real WebSocket connection
            if ((s.level > kC4Connecting && s.error.code == 0)
                    || (s.level == kC4Stopped && s.error.domain == WebSocketDomain))
                Assert(_headers);
        }

        if (s.level == kC4Idle && _stopWhenIdle) {
            C4Log("*** Replicator idle; stopping...");
            c4repl_stop(r);
        }
    }

    static void onStateChanged(C4Replicator *replicator,
                               C4ReplicatorStatus status,
                               void *context)
    {
        ((ReplicatorAPITest*)context)->stateChanged(replicator, status);
    }

    static void onDocsEnded(C4Replicator *repl,
                            bool pushing,
                            size_t nDocs,
                            const C4DocumentEnded* docs[],
                            void *context)
    {
        auto test = (ReplicatorAPITest*)context;
        char message[256];
        test->_docsEnded += nDocs;
        for (size_t i = 0; i < nDocs; ++i) {
            auto doc = docs[i];
            if (doc->error.code) {
                c4error_getDescriptionC(doc->error, message, sizeof(message));
                C4Log(">> Replicator %serror %s '%.*s': %s",
                      (doc->errorIsTransient ? "transient " : ""),
                      (pushing ? "pushing" : "pulling"),
                      SPLAT(doc->docID), message);

                lock_guard<mutex> lock(test->_mutex);
                if (pushing)
                    test->_docPushErrors.emplace(slice(doc->docID));
                else
                    test->_docPullErrors.emplace(slice(doc->docID));
            }
        }
    }


    bool startReplicator(C4ReplicatorMode push, C4ReplicatorMode pull, C4Error *err) {
        _callbackStatus = { };
        _numCallbacks = 0;
        memset(_numCallbacksWithLevel, 0, sizeof(_numCallbacksWithLevel));
        _docPushErrors = _docPullErrors = { };
        _docsEnded = 0;

        if (push > kC4Passive && (slice(_remoteDBName).hasPrefix("scratch"_sl))
                && !db2 && !_flushedScratch) {
            flushScratchDatabase();
        }

        C4ReplicatorParameters params = {};
        params.push = push;
        params.pull = pull;
        params.optionsDictFleece = _options.data();
        params.pushFilter = _pushFilter;
//        params.validationFunc = onValidate;
        params.onStatusChanged = onStateChanged;
        params.onDocumentsEnded = onDocsEnded;
        params.callbackContext = this;
        params.socketFactory = _socketFactory;

        _repl = c4repl_new(db, _address, _remoteDBName,
                           (_remoteDBName.buf ? nullptr : (C4Database*)db2),
                           params, err);
        return (_repl != nullptr);
    }

    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull, bool expectSuccess =true) {
        C4Error err;
        REQUIRE(startReplicator(push, pull, &err));
        C4ReplicatorStatus status = c4repl_getStatus(_repl);
        logState(status);
        // Sometimes Windows goes so fast that by the time
        // it is here, it's already past the connecting stage
        CHECK((status.level == kC4Connecting || status.level == kC4Busy)); 
        CHECK(status.error.code == 0);

        while ((status = c4repl_getStatus(_repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        lock_guard<mutex> lock(_mutex);

        CHECK(_numCallbacks > 0);
        if (expectSuccess) {
            CHECK(status.error.code == 0);
            CHECK(_numCallbacksWithLevel[kC4Busy] > 0);
            if (!db2)
                CHECK(_headers);
        }
        CHECK(_numCallbacksWithLevel[kC4Stopped] == 1);
        CHECK(_callbackStatus.level == status.level);
        CHECK(_callbackStatus.error.domain == status.error.domain);
        CHECK(_callbackStatus.error.code == status.error.code);
        CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));
    }


    alloc_slice sendRemoteRequest(const string &method,
                                  string path,
                                  slice body =nullslice,
                                  bool admin =false)
    {
        REQUIRE(slice(_remoteDBName).hasPrefix("scratch"_sl));

        auto port = uint16_t(_address.port + !!admin);
        path = string("/") + (string)(slice)_remoteDBName + "/" + path;
        if (_logRemoteRequests)
            C4Log("*** Server command: %s %.*s:%d%s",
              method.c_str(), SPLAT(_address.hostname), port, path.c_str());

        Encoder enc;
        enc.beginDict();
        enc["Content-Type"_sl] = "application/json";
        enc.endDict();
        auto headers = enc.finish();

        auto r = make_unique<REST::Response>(method,
                             (string)(slice)_address.hostname,
                             port,
                             path,
                             headers,
                             body);
        REQUIRE(r);
        if (r->error().code)
            FAIL("Error: " << c4error_descriptionStr(r->error()));
        INFO("Status: " << (int)r->status() << " " << r->statusMessage());
        REQUIRE(r->status() >= REST::HTTPStatus::OK);
        REQUIRE(r->status() <= REST::HTTPStatus::Created);
        return r->body();
    }


    void flushScratchDatabase() {
        sendRemoteRequest("POST", "_flush", nullslice, true);
        _flushedScratch = true;
    }


    static vector<string> asVector(const set<string> strings) {
        vector<string> out;
        for (const string &s : strings)
            out.push_back(s);
        return out;
    }


    c4::ref<C4Database> db2;
    C4Address _address = kDefaultAddress;
    C4String _remoteDBName = kScratchDBName;
    AllocedDict _options;
    C4ReplicatorValidationFunction _pushFilter {nullptr};
    C4SocketFactory* _socketFactory {nullptr};
    bool _flushedScratch {false};
    c4::ref<C4Replicator> _repl;

    mutex _mutex;
    C4ReplicatorStatus _callbackStatus {};
    int _numCallbacks {0};
    int _numCallbacksWithLevel[5] {0};
    AllocedDict _headers;
    bool _stopWhenIdle {false};
    int _docsEnded {0};
    set<string> _docPushErrors, _docPullErrors;
    set<string> _expectedDocPushErrors, _expectedDocPullErrors;
    int _counter {0};
    bool _logRemoteRequests {true};
};

