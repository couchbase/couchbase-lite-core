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
#include <algorithm>
#include <chrono>
#include <functional>

using namespace litecore::net;

namespace c4Internal {


    /** A replicator with a remote database via WebSockets. */
    class C4RemoteReplicator : public C4Replicator {
    public:

        // Default maximum number of retry attempts before replications give up.
        // These can be overridden by setting the option `kC4ReplicatorOptionMaxAttempts`, which is MaxRetryCount + 1.
        static constexpr unsigned kMaxOneShotRetryCount = 9;
        static constexpr unsigned kMaxContinuousRetryCount = UINT_MAX;

        // Longest possible retry delay, in seconds. The delay doubles on each failed retry
        // attempt, but pins to this value.
        // This can be overridden by setting the option `kC4ReplicatorOptionMaxAttemptWaitTime`.
        static constexpr unsigned kDefaultMaxRetryDelay = 5 * 60;

        C4RemoteReplicator(C4Database* db NONNULL,
                           const C4ReplicatorParameters &params,
                           const C4Address &serverAddress,
                           C4String remoteDatabaseName)
        :C4Replicator(db, params)
        ,_url(effectiveURL(serverAddress, remoteDatabaseName))
        ,_retryTimer(std::bind(&C4RemoteReplicator::retry, this, false, nullptr))
        {
            if (params.socketFactory) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _customSocketFactory = *params.socketFactory;
                _socketFactory = &_customSocketFactory;
            }
        }


        void start(bool reset) override {
            LOCK(_mutex);
            if (_replicator)
                return;
            _retryCount = 0;
            if(!_restart(reset)) {
                UNLOCK();
                notifyStateChanged();
            }
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
            if(!_restart(false)) {
                UNLOCK();
                notifyStateChanged();
                return false;
            }
            
            return true;
        }


        virtual void stop() override {
            cancelScheduledRetry();
            C4Replicator::stop();
        }


        // Called by the client when it determines the remote host is [un]reachable.
        virtual void setHostReachable(bool reachable) override {
            LOCK(_mutex);
            if (!setStatusFlag(kC4HostReachable, reachable))
                return;
            logInfo("Notified that server is now %sreachable", (reachable ? "" : "un"));
            if (reachable)
                maybeScheduleRetry();
            else
                cancelScheduledRetry();
        }


        virtual void _suspend() override {
            // called with _mutex locked
            cancelScheduledRetry();
            C4Replicator::_suspend();
        }


        virtual bool _unsuspend() override {
            // called with _mutex locked
            maybeScheduleRetry();
            return true;
        }


    protected:
        virtual alloc_slice URL() const override {
            return _url;
        }


        virtual bool createReplicator() override {
            auto webSocket = CreateWebSocket(_url, socketOptions(), _database, _socketFactory);
            
            C4Error err;
            c4::ref<C4Database> dbCopy = c4db_openAgain(_database, &err);
            if(!dbCopy) {
                _status.error = err;
                return false;
            }
            
            _replicator = new Replicator(dbCopy, webSocket, *this, _options);
            
            // Yes this line is disgusting, but the memory addresses that the logger logs
            // are not the _actual_ addresses of the object, but rather the pointer to
            // its Logging virtual table since inside of _logVerbose this is all that
            // is known.
            _logVerbose("C4RemoteRepl %p created Repl %p", (Logging *)this, (Logging *)_replicator.get());
            return true;
        }


        // Both `start` and `retry` end up calling this.
        bool _restart(bool reset) {
            cancelScheduledRetry();
            return _start(reset);
        }


        void maybeScheduleRetry() {
            if (_status.level == kC4Offline &&  statusFlag(kC4HostReachable)
                                            && !statusFlag(kC4Suspended)) {
                _retryCount = 0;
                scheduleRetry(0);
            }
        }


        // Starts the timer to call `retry` in the future.
        void scheduleRetry(unsigned delayInSecs) {
            _retryTimer.fireAfter(std::chrono::seconds(delayInSecs));
            setStatusFlag(kC4WillRetry, true);
        }


        // Cancels a previous call to `scheduleRetry`.
        void cancelScheduledRetry() {
            _retryTimer.stop();
            setStatusFlag(kC4WillRetry, false);
        }


        // Overridden to clear the retry count, so that after a disconnect we'll get more retries.
        virtual void handleConnected() override {
            _retryCount = 0;
        }


        // Overridden to handle transient or network-related errors and possibly retry.
        virtual void handleStopped() override {
            C4Error c4err = _status.error;
            if (c4err.code == 0)
                return;

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

                if (transient || statusFlag(kC4HostReachable)) {
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


        // The function governing the exponential backoff of retries
        unsigned retryDelay(unsigned retryCount) const {
            unsigned delay = 1 << std::min(retryCount, 30u);
            unsigned maxDelay = getIntProperty(kC4ReplicatorOptionMaxRetryInterval,
                                               kDefaultMaxRetryDelay);
            maxDelay = getIntProperty(kC4ReplicatorOptionMaxAttemptWaitTime, maxDelay);
            if (maxDelay == 0) {
                maxDelay = kDefaultMaxRetryDelay;
            }
            return std::min(delay, maxDelay);
        }


        // Returns the maximum number of (failed) retry attempts.
        unsigned maxRetryCount() const {
            unsigned defaultCount = continuous() ? kMaxContinuousRetryCount : kMaxOneShotRetryCount;
            unsigned ret = getIntProperty(kC4ReplicatorOptionMaxRetries, defaultCount);
            ret = getIntProperty(kC4ReplicatorOptionMaxAttempts, ret + 1);
            return ret == 0 ? defaultCount : ret - 1;
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
            string protocolString = string(blip::Connection::kWSProtocolName) + kReplicatorProtocolName;
            Replicator::Options opts(kC4Disabled, kC4Disabled, _options.properties);
            opts.setProperty(slice(kC4SocketOptionWSProtocols), protocolString.c_str());
            return opts.properties.data();
        }


    private:
        alloc_slice const       _url;
        const C4SocketFactory*  _socketFactory {nullptr};
        C4SocketFactory         _customSocketFactory {};  // Storage for *_socketFactory if non-null
        litecore::actor::Timer  _retryTimer;
        unsigned                _retryCount {0};
    };

}
