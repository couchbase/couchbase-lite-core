//
//  ReplicatorAPITest.hh
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
#include "c4ReplicatorTypes.h"
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include "c4Certificate.h"
#include "c4Replicator.h"
#include "Address.hh"
#include "Response.hh"
#include "ReplicatorTuning.hh"
#include "c4Test.hh"
#include "StringUtil.hh"
#include "ReplParams.hh"
#include "SG.hh"
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace fleece;
using namespace litecore;
using namespace litecore::net;

class ReplicatorAPITest : public C4Test {
  public:
    constexpr static slice kDB2Name = "cbl_core_test2.cblite2";

    // Default address to replicate with (individual tests can override this):
    constexpr static const C4Address kDefaultAddress{kC4Replicator2Scheme, C4STR("localhost"), 4984};
    // Common remote (SG) database names:
    constexpr static const C4String kScratchDBName     = C4STR("scratch");
    constexpr static const C4String kITunesDBName      = C4STR("itunes");
    constexpr static const C4String kWikipedia1kDBName = C4STR("wikipedia1k");
    constexpr static const C4String kProtectedDBName   = C4STR("seekrit");
    constexpr static const C4String kImagesDBName      = C4STR("images");

    static std::once_flag once;

    ReplicatorAPITest() : ReplicatorAPITest(0) {}

    explicit ReplicatorAPITest(int option) : C4Test(option), _sg({kDefaultAddress, kScratchDBName}) {
        std::call_once(once, [&]() {
            // Register the BuiltInWebSocket class as the C4Replicator's WebSocketImpl.
            C4RegisterBuiltInWebSocket();
        });
        C4Address address{kDefaultAddress};
        // Environment variables can also override the default address above:
        if ( getenv("REMOTE_TLS") || getenv("REMOTE_SSL") ) address.scheme = kC4Replicator2TLSScheme;
        const char* hostname = getenv("REMOTE_HOST");
        if ( hostname ) address.hostname = c4str(hostname);
        const char* portStr = getenv("REMOTE_PORT");
        if ( portStr ) address.port = (uint16_t)strtol(portStr, nullptr, 10);
        const char* remoteDB = getenv("REMOTE_DB");
        if ( remoteDB ) _sg.remoteDBName = c4str(remoteDB);
        const char* proxyURL = getenv("REMOTE_PROXY");
        if ( proxyURL ) { _sg.proxy = std::make_shared<ProxySpec>(Address(slice(proxyURL))); }

        _sg.address = address;

        if ( Address::isSecure(_sg.address) ) { _sg.pinnedCert = readFile(sReplicatorFixturesDir + "cert/cert.pem"); }

        _onDocsEnded    = onDocsEnded;
        _onlySelfSigned = false;
    }

#ifdef COUCHBASE_ENTERPRISE
    // Create an empty database db2 and make it the target of the replication
    void createDB2() {
        auto    config = c4db_getConfig2(db);
        C4Error error;
        if ( !c4db_deleteNamed(kDB2Name, config->parentDirectory, ERROR_INFO(error)) ) REQUIRE(error.code == 0);
        db2 = c4db_openNamed(kDB2Name, config, ERROR_INFO(error));
        REQUIRE(db2 != nullptr);

        _sg.address      = {};
        _sg.remoteDBName = nullslice;
    }
#endif

    AllocedDict options() {
        Encoder enc;
        enc.beginDict();
        if ( _sg.pinnedCert ) {
            enc.writeKey(C4STR(kC4ReplicatorOptionPinnedServerCert));
            enc.writeData(_sg.pinnedCert);
        }
#ifdef COUCHBASE_ENTERPRISE
        if ( _sg.identityCert ) {
            enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
            enc.beginDict();
            enc[C4STR(kC4ReplicatorAuthType)] = kC4AuthTypeClientCert;
            enc.writeKey(C4STR(kC4ReplicatorAuthClientCert));
            enc.writeData(alloc_slice(c4cert_copyData(_sg.identityCert, false)));
            alloc_slice privateKeyData(c4keypair_privateKeyData(_sg.identityKey));
            if ( privateKeyData ) {
                enc.writeKey(C4STR(kC4ReplicatorAuthClientCertKey));
                enc.writeData(privateKeyData);
            }
            enc.endDict();
        }
#endif

        if ( _customCaCert.buf ) {
            enc.writeKey(C4STR(kC4ReplicatorOptionRootCerts));
            enc.writeData(_customCaCert);
        }

        if ( _onlySelfSigned ) {
            enc.writeKey(C4STR(kC4ReplicatorOptionOnlySelfSignedServerCert));
            enc.writeBool(true);
        }

        if ( _sg.networkInterface ) {
            enc.writeKey(C4STR(kC4SocketOptionNetworkInterface));
            enc.writeString(_sg.networkInterface);
        }

        if ( _sg.proxy ) {
            enc.writeKey(C4STR(kC4ReplicatorProxyType));
            slice proxyType;
            switch ( _sg.proxy->type ) {
                case ProxyType::HTTP:
                    proxyType = slice(kC4ProxyTypeHTTP);
                case ProxyType::HTTPS:
                    proxyType = slice(kC4ProxyTypeHTTPS);
                // If we ever support SOCKS
                // case ProxyType::SOCKS:
                //     proxyType = slice(kC4ProxyTypeSOCKS);
                default:
                    proxyType = kC4ProxyTypeNone;
            }
            enc.writeString(proxyType);
            if ( proxyType != slice(kC4ProxyTypeNone) ) {
                enc.writeKey(kC4ReplicatorProxyHost);
                enc.writeString(_sg.proxy->hostname);

                enc.writeKey(kC4ReplicatorProxyPort);
                enc.writeInt(_sg.proxy->port);

                enc.writeKey(kC4ReplicatorProxyAuth);
                enc.beginDict();

                enc.writeKey(kC4ReplicatorAuthType);
                enc.writeString(kC4AuthTypeBasic);

                enc.writeKey(kC4ReplicatorAuthUserName);
                enc.writeString(_sg.proxy->username);

                enc.writeKey(kC4ReplicatorAuthPassword);
                enc.writeString(_sg.proxy->password);

                enc.endDict();
            }
        }
        // Copy any preexisting options:
        for ( Dict::iterator i(_options.asDict()); i; ++i ) {
            enc.writeKey(i.keyString());
            enc.writeValue(i.value());
        }
        enc.endDict();
        return AllocedDict(enc.finish());
    }

    static void logState(C4ReplicatorStatus status) {
        std::string flags{};
        if ( status.flags & kC4WillRetry ) flags += "retry,";
        if ( status.flags & kC4HostReachable ) flags += "reachable,";
        if ( status.flags & kC4Suspended ) flags += "suspended,";
        if ( status.error.code ) {
            char message[200];
            c4error_getDescriptionC(status.error, message, sizeof(message));
            C4Log("*** C4Replicator state: %-s (%s), progress=%" PRIu64 "/%" PRIu64 ", error=%s",
                  kC4ReplicatorActivityLevelNames[status.level], flags.c_str(), status.progress.unitsCompleted,
                  status.progress.unitsTotal, message);
        } else {
            C4Log("*** C4Replicator state: %-s (%s), progress=%" PRIu64 "/%" PRIu64,
                  kC4ReplicatorActivityLevelNames[status.level], flags.c_str(), status.progress.unitsCompleted,
                  status.progress.unitsTotal);
        }
    }

    void stateChanged(C4Replicator* r, C4ReplicatorStatus s) {
        std::unique_lock<std::mutex> lock(_mutex);

        logState(s);
        if ( r != _repl ) {
            WARN("Stray stateChange received, check C4Log for details!");
            C4Warn("Stray stateChanged message received (possibly from previous test?): (r = %p, _repl = %p)", r,
                   (C4Replicator*)_repl);
            return;
        }

        _callbackStatus = s;
        ++_numCallbacks;
        Require(s.level != kC4Stopping);  // No internal state allowed
        _numCallbacksWithLevel[(int)s.level]++;
        if ( s.level == kC4Offline ) {
            Require(_mayGoOffline);
            _wentOffline = true;
            Check(asVector(_docPullErrors) == asVector(_expectedDocPullErrorsAfterOffline));
            Check(asVector(_docPushErrors) == asVector(_expectedDocPushErrorsAfterOffline));
            _docPullErrors.clear();
            _docPushErrors.clear();
        }

#ifdef COUCHBASE_ENTERPRISE
        if ( !_sg.remoteCert ) {
            C4Error err;
            _sg.remoteCert = c4cert_retain(c4repl_getPeerTLSCertificate(_repl, &err));
            if ( !_sg.remoteCert && err.code != 0 ) {
                WARN("Failed to get remote TLS certificate: error " << err.domain << "/" << err.code);
                Check(err.code == 0);
            }
        }
#endif

        if ( !_headers ) {
            _headers = AllocedDict(alloc_slice(c4repl_getResponseHeaders(_repl)));
            if ( !!_headers ) {
                for ( Dict::iterator header(_headers.asDict()); header; ++header )
                    C4Log("    %.*s: %.*s", SPLAT(header.keyString()), SPLAT(header.value().asString()));
            }
        }

        if ( !_socketFactory && !db2 ) {  // i.e. this is a real WebSocket connection
            if ( (s.level > kC4Connecting && s.error.code == 0)
                 || (s.level == kC4Stopped && s.error.domain == WebSocketDomain) )
                Require(_headers);
        }

        if ( s.level == kC4Idle ) {
            if ( _stopWhenIdle.load() ) {
                C4Log("*** Replicator idle; stopping...");
                c4repl_stop(r);
            } else if ( _callbackWhenIdle ) {
                C4Log("*** Replicator idle");
                _callbackWhenIdle();
            }
        }

        _stateChangedCondition.notify_all();
    }

    static void onStateChanged(C4Replicator* replicator, C4ReplicatorStatus status, void* context) {
        ((ReplicatorAPITest*)context)->stateChanged(replicator, status);
    }

    static void onDocsEnded(C4Replicator* repl, bool pushing, size_t nDocs, const C4DocumentEnded* docs[],
                            void* context) {
        auto                         test = (ReplicatorAPITest*)context;
        std::unique_lock<std::mutex> lock(test->_mutex);

        char message[256];
        test->_docsEnded += (int)nDocs;
        for ( size_t i = 0; i < nDocs; ++i ) {
            auto doc = docs[i];
            if ( doc->error.code ) {
                c4error_getDescriptionC(doc->error, message, sizeof(message));
                C4Warn(">> Replicator %serror %s '%.*s': %s", (doc->errorIsTransient ? "transient " : ""),
                       (pushing ? "pushing" : "pulling"), SPLAT(doc->docID), message);

                if ( pushing ) test->_docPushErrors.emplace(slice(doc->docID));
                else if ( doc->error.domain == LiteCoreDomain && doc->error.code == kC4ErrorConflict
                          && test->_conflictHandler ) {
                    test->_conflictHandler(doc);
                } else {
                    test->_docPullErrors.emplace(slice(doc->docID));
                }
            }
        }
    }

    using PushPull       = std::pair<C4ReplicatorMode, C4ReplicatorMode>;
    using C4ParamsSetter = std::function<void(C4ReplicatorParameters&)>;

    bool startReplicator(const std::variant<PushPull, C4ParamsSetter>& varParams, C4Error* err) {
        if ( !_prepareReplicator(varParams, err) ) { return false; }
        c4repl_start(_repl, false);
        return true;
    }

    bool startReplicator(C4ReplicatorMode push, C4ReplicatorMode pull, C4Error* err) {
        std::variant<PushPull, C4ParamsSetter> varParams = std::make_pair(push, pull);
        return startReplicator(varParams, err);
    }

    bool _prepareReplicator(const std::variant<PushPull, C4ParamsSetter>& varParams, C4Error* err) {
        std::scoped_lock<std::mutex> lock(_mutex);

        _callbackStatus = {};
        _numCallbacks   = 0;
        memset(_numCallbacksWithLevel, 0, sizeof(_numCallbacksWithLevel));
        _docPushErrors = _docPullErrors = {};
        _docsEnded                      = 0;
        _wentOffline                    = false;

        C4ReplicatorMode push{kC4Disabled}, pull{kC4Disabled};
        auto             pp = std::get_if<PushPull>(&varParams);
        if ( pp ) { std::tie(push, pull) = *pp; }

        if ( push > kC4Passive && (slice(_sg.remoteDBName).hasPrefix("scratch"_sl)) && !db2 && !_flushedScratch ) {
            flushScratchDatabase();
        }

        C4ReplicatorParameters params = {};
        _options                      = options();
        params.optionsDictFleece      = _options.data();
        params.onStatusChanged        = onStateChanged;
        params.onDocumentsEnded       = _onDocsEnded;
        params.callbackContext        = this;
        params.socketFactory          = _socketFactory;
        C4ReplicationCollection coll;
        if ( pp ) {
            // Explicit Push/Pull for one collection
            coll                                  = {kC4DefaultCollectionSpec, push, pull};
            params.collections                    = &coll;
            params.collectionCount                = 1;
            params.collections[0].pushFilter      = _pushFilter;
            params.collections[0].pullFilter      = _pullFilter;
            params.collections[0].callbackContext = this;
        } else {
            // Caller will set the Params
            auto pParamsSetter = std::get_if<C4ParamsSetter>(&varParams);
            assert(pParamsSetter);
            (*pParamsSetter)(params);
        }

        if ( _sg.remoteDBName.buf ) {
            _repl = c4repl_new(db, _sg.address, _sg.remoteDBName, params, C4STR("apiTest"), err);
        } else {
#ifdef COUCHBASE_ENTERPRISE
            _repl = c4repl_newLocal(db, db2, params, C4STR("apiTest"), err);
#else
            FAIL("Local replication not supported in CE");
#endif
        }
        if ( !_repl ) return false;

        if ( _enableBlobProgressNotifications ) {
            REQUIRE(c4repl_setProgressLevel(_repl, kC4ReplProgressPerAttachment, err));
        } else if ( _enableDocProgressNotifications ) {
            REQUIRE(c4repl_setProgressLevel(_repl, kC4ReplProgressPerDocument, err));
        }

        return true;
    }

    static constexpr auto kDefaultWaitTimeout = repl::tuning::kDefaultCheckpointSaveDelay + 2s;

    void waitForStatus(C4ReplicatorActivityLevel level, std::chrono::milliseconds timeout = kDefaultWaitTimeout) {
        std::unique_lock<std::mutex> lock(_mutex);
        _waitForStatus(lock, level, timeout);
    }

    void _waitForStatus(std::unique_lock<std::mutex>& lock, C4ReplicatorActivityLevel level,
                        std::chrono::milliseconds timeout = kDefaultWaitTimeout) {
        _stateChangedCondition.wait_for(lock, timeout, [&] { return _numCallbacksWithLevel[level] > 0; });
        if ( _numCallbacksWithLevel[level] == 0 ) FAIL("Timed out waiting for a status callback of level " << level);
    }

    void replicate(ReplParams& params, bool expectSuccess = true) { replicate(params.paramSetter(), expectSuccess); }

    void replicate(const std::variant<PushPull, C4ParamsSetter>& params, bool expectSuccess = true) {
        if ( !startReplicator(params, &_errorBeforeStart) ) {
            CHECK(_repl == nullptr);
            if ( expectSuccess ) { CHECK(_errorBeforeStart.code == 0); }
            return;
        }

        std::unique_lock<std::mutex> lock(_mutex);
        _waitForStatus(lock, kC4Stopped, std::chrono::minutes(5));

        C4ReplicatorStatus status = c4repl_getStatus(_repl);
        if ( expectSuccess ) {
            CHECK(status.error.code == 0);
            CHECK(_numCallbacksWithLevel[kC4Busy] > 0);
            if ( !db2 ) CHECK(!!_headers);
        }
        CHECK(_numCallbacksWithLevel[kC4Stopped] == 1);
        CHECK(_callbackStatus.level == status.level);
        CHECK(_callbackStatus.error.domain == status.error.domain);
        CHECK(_callbackStatus.error.code == status.error.code);
        CHECK(asVector(_docPullErrors) == asVector(_expectedDocPullErrors));
        CHECK(asVector(_docPushErrors) == asVector(_expectedDocPushErrors));

        _repl = nullptr;
    }

    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull, bool expectSuccess = true) {
        std::variant<PushPull, C4ParamsSetter> varParams = std::make_pair(push, pull);
        replicate(varParams, expectSuccess);
    }

    void flushScratchDatabase() {
        _sg.flushDatabase();
        _flushedScratch = true;
    }

    bool requireSG3() const {
        alloc_slice serverName{_sg.getServerName()};
        Require(serverName.hasPrefix("Couchbase Sync Gateway/"));
        if ( serverName >= "Couchbase Sync Gateway/3" ) {
            return true;
        } else {
            C4Warn("*** Skipping test: server is %.*s, but this test requires SG 3.0 or later ***", SPLAT(serverName));
            return false;
        }
    }

    static std::vector<std::string> asVector(const std::set<std::string>& strings) {
        std::vector<std::string> out;
        out.reserve(strings.size());
        for ( const std::string& s : strings ) out.push_back(s);
        return out;
    }

    c4::ref<C4Database>                         db2;
    AllocedDict                                 _options;
    bool                                        _enableDocProgressNotifications{false};
    bool                                        _enableBlobProgressNotifications{false};
    C4ReplicatorValidationFunction              _pushFilter{nullptr};
    C4ReplicatorValidationFunction              _pullFilter{nullptr};
    C4ReplicatorDocumentsEndedCallback          _onDocsEnded{nullptr};
    std::function<void(const C4DocumentEnded*)> _conflictHandler{nullptr};
    C4SocketFactory*                            _socketFactory{nullptr};
    bool                                        _flushedScratch{false};
    c4::ref<C4Replicator>                       _repl;

    std::mutex              _mutex;
    std::condition_variable _stateChangedCondition;
    C4ReplicatorStatus      _callbackStatus{};
    C4Error                 _errorBeforeStart{LiteCoreDomain, 0};
    int                     _numCallbacks{0};
    int                     _numCallbacksWithLevel[5]{0};
    AllocedDict             _headers;
    std::atomic<bool>       _stopWhenIdle{false};
    std::function<void()>   _callbackWhenIdle;
    int                     _docsEnded{0};
    std::set<std::string>   _docPushErrors, _docPullErrors;
    std::set<std::string>   _expectedDocPushErrors, _expectedDocPullErrors;
    int                     _counter{0};
    bool                    _mayGoOffline{false};
    bool                    _wentOffline{false};
    bool                    _onlySelfSigned{false};
    alloc_slice             _customCaCert{};
    SG                      _sg;
    std::set<std::string>   _expectedDocPushErrorsAfterOffline;
    std::set<std::string>   _expectedDocPullErrorsAfterOffline;
};
