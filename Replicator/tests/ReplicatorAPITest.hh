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
#include "c4Certificate.h"
#include "Address.hh"
#include "Response.hh"
#include "c4Test.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::net;


extern "C" {
    void C4RegisterBuiltInWebSocket();
}


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

    static alloc_slice sPinnedCert;

    ReplicatorAPITest()
    :C4Test(0)
    {
        static once_flag once;
        call_once(once, [&]() {
            // Register the BuiltInWebSocket class as the C4Replicator's WebSocketImpl.
            C4RegisterBuiltInWebSocket();

            // Pin the server certificate:
            sPinnedCert = readFile(sReplicatorFixturesDir + "cert.pem");
        });

        // Environment variables can also override the default address above:
        if (getenv("REMOTE_TLS") || getenv("REMOTE_SSL"))
            _address.scheme = C4STR("wss");
        const char *hostname = getenv("REMOTE_HOST");
        if (hostname)
            _address.hostname = c4str(hostname);
        const char *portStr = getenv("REMOTE_PORT");
        if (portStr)
            _address.port = (uint16_t)strtol(portStr, nullptr, 10);
        const char *remoteDB = getenv("REMOTE_DB");
        if (remoteDB)
            _remoteDBName = c4str(remoteDB);
        const char *proxyURL = getenv("REMOTE_PROXY");
        if (proxyURL) {
            Address proxyAddr{slice(proxyURL)};
            _proxy = make_unique<ProxySpec>(proxyAddr);
        }

        if (Address::isSecure(_address)) {
            pinnedCert = sPinnedCert;
        }

        _onDocsEnded = onDocsEnded;
    }

#ifdef COUCHBASE_ENTERPRISE
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
    
    void waitForStatus(C4ReplicatorActivityLevel level, int sleepMs = 100, int attempts = 50) {
        int attempt = 0;
        while (c4repl_getStatus(_repl).level != level && ++attempt < attempts) {
            this_thread::sleep_for(chrono::milliseconds(sleepMs));
        }
        
        CHECK(attempt < attempts);
    }
#endif

    AllocedDict options() {
        Encoder enc;
        enc.beginDict();
        if (pinnedCert) {
            enc.writeKey(C4STR(kC4ReplicatorOptionPinnedServerCert));
            enc.writeData(pinnedCert);
        }
#ifdef COUCHBASE_ENTERPRISE
        if (identityCert) {
            enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
            enc.beginDict();
            enc[C4STR(kC4ReplicatorAuthType)] = kC4AuthTypeClientCert;
            enc.writeKey(C4STR(kC4ReplicatorAuthClientCert));
            enc.writeData(alloc_slice(c4cert_copyData(identityCert, false)));
            alloc_slice privateKeyData(c4keypair_privateKeyData(identityKey));
            if (privateKeyData) {
                enc.writeKey(C4STR(kC4ReplicatorAuthClientCertKey));
                enc.writeData(privateKeyData);
            }
            enc.endDict();
        }
#endif
        if (_enableDocProgressNotifications) {
            enc.writeKey(C4STR(kC4ReplicatorOptionProgressLevel));
            enc.writeInt(1);
        }
        // TODO: Set proxy settings from _proxy
        // Copy any preexisting options:
        for (Dict::iterator i(_options); i; ++i) {
            enc.writeKey(i.keyString());
            enc.writeValue(i.value());
        }
        enc.endDict();
        return AllocedDict(enc.finish());
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
        
        logState(s);
        if(r != _repl) {
            WARN("Stray stateChange received, check C4Log for details!");
            C4Warn("Stray stateChanged message received (possibly from previous test?): (r = %p, _repl = %p)", r, (C4Replicator *)_repl);
            return;
        }

        _callbackStatus = s;
        ++_numCallbacks;
        Assert(s.level != kC4Stopping);   // No internal state allowed
        _numCallbacksWithLevel[(int)s.level]++;
        if (s.level == kC4Busy)
            Assert(s.error.code == 0);                          // Busy state shouldn't have error
        if (s.level == kC4Offline) {
            Assert(_mayGoOffline);
            _wentOffline = true;
        }
        
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
                C4Warn(">> Replicator %serror %s '%.*s': %s",
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
        _wentOffline = false;

        if (push > kC4Passive && (slice(_remoteDBName).hasPrefix("scratch"_sl))
                && !db2 && !_flushedScratch) {
            flushScratchDatabase();
        }

        C4ReplicatorParameters params = {};
        params.push = push;
        params.pull = pull;
        _options = options();
        params.optionsDictFleece = _options.data();
        params.pushFilter = _pushFilter;
//        params.validationFunc = onValidate;
        params.onStatusChanged = onStateChanged;
        params.onDocumentsEnded = _onDocsEnded;
        params.callbackContext = this;
        params.socketFactory = _socketFactory;

        if (_remoteDBName.buf) {
            _repl = c4repl_new(db, _address, _remoteDBName, params, err);
        } else {
#ifdef COUCHBASE_ENTERPRISE
            _repl = c4repl_newLocal(db, db2, params, err);
#else
            FAIL("Local replication not supported in CE");
#endif
        }
        if (!_repl)
            return false;
        c4repl_start(_repl);
        return true;
    }

    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull, bool expectSuccess =true) {
        C4Error err;
        REQUIRE(startReplicator(push, pull, &err));
        C4ReplicatorStatus status = c4repl_getStatus(_repl);
        logState(status);
        // Sometimes Windows goes so fast that by the time
        // it is here, it's already past the connecting stage.
        // Furthermore, sometimes in failure cases the failure happens
        // so fast that the replicator has already stopped by now with an
        // error
        if(expectSuccess) {
            CHECK((status.level == kC4Connecting || status.level == kC4Busy)); 
            CHECK(status.error.code == 0);
        } else if(status.level == kC4Connecting || status.level == kC4Busy) {
            CHECK(status.error.code == 0);
        }

        while ((status = c4repl_getStatus(_repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        int attempts = 0;
        while(_numCallbacksWithLevel[kC4Stopped] != 1 && attempts++ < 5) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        
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

        _repl = nullptr;
    }


    /// Sends an HTTP request to the remote server.
    alloc_slice sendRemoteRequest(const string &method,
                                  string path,
                                  HTTPStatus *outStatus NONNULL,
                                  C4Error *outError NONNULL,
                                  slice body =nullslice,
                                  bool admin =false)
    {
        if (method != "GET")
            REQUIRE(slice(_remoteDBName).hasPrefix("scratch"_sl));

        auto port = uint16_t(_address.port + !!admin);
        if (!hasPrefix(path, "/"))
            path = string("/") + (string)(slice)_remoteDBName + "/" + path;
        if (_logRemoteRequests)
            C4Log("*** Server command: %s %.*s:%d%s",
              method.c_str(), SPLAT(_address.hostname), port, path.c_str());

        Encoder enc;
        enc.beginDict();
        enc["Content-Type"_sl] = "application/json";
        enc.endDict();
        auto headers = enc.finishDoc();

        string scheme = Address::isSecure(_address) ? "https" : "http";
        auto r = make_unique<REST::Response>(scheme,
                                             method,
                                             (string)(slice)_address.hostname,
                                             port,
                                             path);
        r->setHeaders(headers).setBody(body).setTimeout(5);
        if (pinnedCert)
            r->allowOnlyCert(pinnedCert);
        if (_authHeader)
            r->setAuthHeader(_authHeader);
        if (_proxy)
            r->setProxy(*_proxy);
#ifdef COUCHBASE_ENTERPRISE
        if (identityCert)
            r->setIdentity(identityCert, identityKey);
#endif

        if (r->run()) {
            *outStatus = r->status();
            *outError = {};
            return r->body();
        } else {
            REQUIRE(r->error().code != 0);
            *outStatus = HTTPStatus::undefined;
            *outError = r->error();
            return nullslice;
        }
    }


    /// Sends an HTTP request to the remote server.
    alloc_slice sendRemoteRequest(const string &method,
                                  string path,
                                  slice body =nullslice,
                                  bool admin =false,
                                  HTTPStatus expectedStatus = HTTPStatus::OK)
    {
        if (method == "PUT" && expectedStatus == HTTPStatus::OK)
            expectedStatus = HTTPStatus::Created;
        HTTPStatus status;
        C4Error error;
        alloc_slice response = sendRemoteRequest(method, path, &status, &error, body, admin);
        if (error.code)
            FAIL("Error: " << c4error_descriptionStr(error));
        INFO("Status: " << (int)status);
        REQUIRE(status == expectedStatus);
        return response;
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
    alloc_slice _authHeader;
    alloc_slice pinnedCert;
#ifdef COUCHBASE_ENTERPRISE
    c4::ref<C4Cert> identityCert;
    c4::ref<C4KeyPair> identityKey;
#endif
    unique_ptr<ProxySpec> _proxy;
    bool _enableDocProgressNotifications {false};
    C4ReplicatorValidationFunction _pushFilter {nullptr};
    C4ReplicatorDocumentsEndedCallback _onDocsEnded {nullptr};
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
    bool _mayGoOffline {false};
    bool _wentOffline {false};
};

