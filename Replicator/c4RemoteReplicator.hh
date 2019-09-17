//
//  c4RemoteReplicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Replicator.hh"
#include "c4Socket+Internal.hh"
#include "c4.hh"
#include "Address.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include <chrono>
#include <functional>

using namespace litecore::net;

namespace c4Internal {


    /** A replicator with a remote database via WebSockets. */
    class C4RemoteReplicator : public C4Replicator {
    public:

        // Maximum number of retries before a one-shot replication gives up
        static constexpr unsigned kMaxOneShotRetryCount = 2;

        // Longest possible retry delay, in seconds (only a continuous replication will reach this.)
        // But a call to c4repl_retry() will also trigger a retry.
        static constexpr unsigned kMaxRetryDelay = 10 * 60;

        // The function governing the exponential backoff of retries
        static unsigned retryDelay(unsigned retryCount) {
            unsigned delay = 1 << std::min(retryCount, 30u);
            return std::min(delay, kMaxRetryDelay);
        }


        C4RemoteReplicator(C4Database* db,
                           const C4ReplicatorParameters &params,
                           const C4Address &serverAddress,
                           C4String remoteDatabaseName)
        :C4Replicator(db, params)
        ,_url(effectiveURL(serverAddress, remoteDatabaseName))
        ,_retryTimer(std::bind(&C4RemoteReplicator::retry, this, false, nullptr))
        { }


        void start(bool synchronous =false) override {
            LOCK(_mutex);
            if (_replicator)
                return;
            _retryCount = 0;
            _restart(synchronous);
        }


        virtual bool retry(bool resetCount, C4Error *outError) override {
            LOCK(_mutex);
            if (resetCount)
                _retryCount = 0;
            if (_status.level >= kC4Connecting)
                return true;
            if (_status.level == kC4Stopped) {
                c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                               "Replicator is stopped"_sl, outError);
                return false;
            }
            logInfo("Retrying connection to %.*s (attempt #%u)...", SPLAT(_url), _retryCount+1);
            _restart();
            return true;
        }


        virtual bool willRetry() const override {
            return _retryTimer.scheduled();
        }


        virtual void stop() override {
            cancelScheduledRetry();
            C4Replicator::stop();
        }


        // Called by the client when it determines the remote host is [un]reachable.
        virtual void setHostReachable(bool reachable) override {
            LOCK(_mutex);
            if (reachable == _reachable)
                return;
            _reachable = reachable;
            logInfo("Notified server is now %sreachable", (reachable ? "" : "un"));
            if (!reachable) {
                cancelScheduledRetry();
            } else if (_status.level == kC4Offline) {
                _retryCount = 0;
                scheduleRetry(0);
            }
        }


    protected:
        // Both `start` and `retry` end up calling this.
        void _restart(bool synchronous =false) {
            cancelScheduledRetry();
            auto webSocket = CreateWebSocket(_url, socketOptions(), _database, _params.socketFactory);
            _start(new Replicator(_database, webSocket, *this, options()),
                   synchronous);
        }


        // Starts the timer to call `retry` in the future.
        void scheduleRetry(unsigned delayInSecs) {
            _retryTimer.fireAfter(chrono::seconds(delayInSecs));
        }


        // Cancels a previous call to `scheduleRetry`.
        void cancelScheduledRetry() {
            _retryTimer.stop();
        }


        // Overridden to clear the retry count, so that after a disconnect we'll get more retries.
        virtual void handleConnected() override {
            _retryCount = 0;
        }


        // Overridden to handle transient or network-related errors and possibly retry.
        virtual void handleError(C4Error c4err) override {
            // If this is a transient error, or if I'm continuous and the error might go away with
            // a change in network (i.e. network down, hostname unknown), then go offline.
            bool transient = c4error_mayBeTransient(c4err);
            if (transient || (continuous() && c4error_mayBeNetworkDependent(c4err))) {
                if (_retryCount >= maxRetryCount()) {
                    logError("Will not retry; max retry count (%u) reached", _retryCount);
                    return;
                }

                // OK, we are going offline, to retry later:
                _status.level = kC4Offline;
                _replicator = nullptr;

                if (transient || _reachable) {
                    // On transient error, retry periodically, with exponential backoff:
                    unsigned delay = retryDelay(++_retryCount);
                    logError("Transient error (%s); attempt #%u in %u sec...",
                             c4error_descriptionStr(c4err), _retryCount+1, delay);
                    scheduleRetry(delay);
                } else {
                    // On other network error, don't retry automatically. The client should await
                    // a network change and call c4repl_retry.
                    logError("Network error (%s); will retry when host becomes reachable...",
                             c4error_descriptionStr(c4err));
                }
            }
        }


        // Returns the maximum number of (failed) retry attempts.
        unsigned maxRetryCount() const {
            auto maxRetries = options().properties[kC4ReplicatorOptionMaxRetries];
            if (maxRetries.type() == kFLNumber)
                return unsigned(maxRetries.asUnsigned());
            else if (continuous())
                return UINT_MAX;
            else
                return kMaxOneShotRetryCount;
        }


        // Returns URL string with the db name and "/_blipsync" appended to the Address's path
        static alloc_slice effectiveURL(C4Address address, slice remoteDatabaseName) {
            slice path = address.path;
            string newPath = string(path);
            if (!path.hasSuffix("/"_sl))
                newPath += "/";
            newPath += string(remoteDatabaseName) + "/_blipsync";
            address.path = slice(newPath);
            return Address::toURL(address);
        }


        // Options to pass to the C4Socket
        alloc_slice socketOptions() const {
            string protocolString = string(Connection::kWSProtocolName) + kReplicatorProtocolName;
            Replicator::Options opts(kC4Disabled, kC4Disabled, _params.optionsDictFleece);
            opts.setProperty(slice(kC4SocketOptionWSProtocols), protocolString.c_str());
            return opts.properties.data();
        }


    private:
        alloc_slice const       _url;
        litecore::actor::Timer  _retryTimer;
        unsigned                _retryCount {0};
        bool                    _reachable {true};
    };

}
