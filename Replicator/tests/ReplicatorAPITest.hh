//
//  ReplicatorAPITest.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/12/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
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

    ReplicatorAPITest()
    :C4Test(0)
    {
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
        auto db2Path = TempDir() + "cbl_core_test2";
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

    bool validate(slice docID, Dict body) {
        //TODO: Do something here
        return true;
    }

    static bool onValidate(FLString docID, FLDict body, void *context) {
        return ((ReplicatorAPITest*)context)->validate(docID, body);
    }

    void logState(C4ReplicatorStatus status) {
        char message[200];
        c4error_getMessageC(status.error, message, sizeof(message));
        C4Log("*** C4Replicator state: %-s, progress=%llu/%llu, error=%d/%d: %s",
              kC4ReplicatorActivityLevelNames[status.level],
              status.progress.unitsCompleted, status.progress.unitsTotal,
              status.error.domain, status.error.code, message);
    }

    void stateChanged(C4Replicator *r, C4ReplicatorStatus s) {
        Assert(r == _repl);      // can't call REQUIRE on a background thread
        _callbackStatus = s;
        ++_numCallbacks;
        _numCallbacksWithLevel[(int)s.level]++;
        logState(_callbackStatus);

        if (!_headers) {
            _headers = AllocedDict(alloc_slice(c4repl_getResponseHeaders(_repl)));
            if (!!_headers) {
                for (Dict::iterator header(_headers); header; ++header)
                    C4Log("    %.*s: %.*s", SPLAT(header.keyString()), SPLAT(header.value().asString()));
            }
        }

        if (!db2) {  // i.e. this is a real WebSocket connection
            if (s.level > kC4Connecting
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

    static void onDocError(C4Replicator *repl,
                           bool pushing,
                           C4String docID,
                           C4Error error,
                           bool transient,
                           void *context)
    {
        char message[256];
        c4error_getMessageC(error, message, sizeof(message));
        C4Log(">> Replicator %serror %s '%.*s': %s",
              (transient ? "transient " : ""),
              (pushing ? "pushing" : "pulling"),
              SPLAT(docID), message);

        auto test = (ReplicatorAPITest*)context;
        if (pushing)
            test->_docPushErrors.emplace(slice(docID));
        else
            test->_docPullErrors.emplace(slice(docID));
    }


    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull, bool expectSuccess =true) {
        if (push > kC4Passive && _remoteDBName == kScratchDBName && !db2 && !_flushedScratch) {
            flushScratchDatabase();
            _flushedScratch = true;
        }

        C4ReplicatorParameters params = {};
        params.push = push;
        params.pull = pull;
        params.optionsDictFleece = _options.data();
        params.validationFunc = onValidate;
        params.onStatusChanged = onStateChanged;
        params.onDocumentError = onDocError;
        params.callbackContext = this;

        C4Error err;
        _repl = c4repl_new(db, _address, _remoteDBName, db2, params, &err);
        REQUIRE(_repl);
        C4ReplicatorStatus status = c4repl_getStatus(_repl);
        logState(status);
        CHECK(status.level == kC4Connecting);
        CHECK(status.error.code == 0);

        while ((status = c4repl_getStatus(_repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        CHECK(_numCallbacks > 0);
        if (expectSuccess) {
            CHECK(status.error.code == 0);
            CHECK(_numCallbacksWithLevel[kC4Busy] > 0);
            //CHECK(_gotHeaders);   //FIX: Enable this when civetweb can return HTTP headers
        }
        CHECK(_numCallbacksWithLevel[kC4Stopped] > 0);
        CHECK(_callbackStatus.level == status.level);
        CHECK(_callbackStatus.error.domain == status.error.domain);
        CHECK(_callbackStatus.error.code == status.error.code);
        CHECK(_docPullErrors.empty());
        CHECK(_docPushErrors.empty());
    }


    void flushScratchDatabase() {
        C4Log("*** Erasing scratch database");
        auto r = make_unique<REST::Response>("POST",
                                             (string)(slice)_address.hostname,
                                             (uint16_t)(_address.port + 1),     // assume this is the admin port
                                             string("/") + (string)(slice)kScratchDBName + "/_flush");
        REQUIRE(r);
        INFO("Status: " << (int)r->status() << " " << r->statusMessage());
        REQUIRE(r->status() == REST::HTTPStatus::OK);
    }

    c4::ref<C4Database> db2;
    C4Address _address {kDefaultAddress};
    C4String _remoteDBName {kScratchDBName};
    AllocedDict _options;
    bool _flushedScratch {false};
    c4::ref<C4Replicator> _repl;
    C4ReplicatorStatus _callbackStatus {};
    int _numCallbacks {0};
    int _numCallbacksWithLevel[5] {0};
    AllocedDict _headers;
    bool _stopWhenIdle {false};
    set<string> _docPushErrors, _docPullErrors;
};

