//
//  c4RemoteReplicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorImpl.hh"
#include "c4Socket+Internal.hh"
#include "Address.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include <algorithm>
#include <chrono>
#include <functional>

using namespace litecore::net;

namespace litecore {


    /** A replicator with a remote database via WebSockets. */
    class C4RemoteReplicator final : public C4ReplicatorImpl {
    public:

        // Default maximum number of retry attempts before replications give up.
        // These can be overridden by setting the option `kC4ReplicatorOptionMaxRetries`.
        static constexpr unsigned kMaxOneShotRetryCount = 9;
        static constexpr unsigned kMaxContinuousRetryCount = UINT_MAX;

        // Longest possible retry delay, in seconds. The delay doubles on each failed retry
        // attempt, but pins to this value.
        // This can be overridden by setting the option `kC4ReplicatorOptionMaxRetryInterval`.
        static constexpr unsigned kDefaultMaxRetryDelay = 5 * 60;

        C4RemoteReplicator(C4Database* db NONNULL,
                           const C4ReplicatorParameters &params,
                           const C4Address &serverAddress,
                           C4String remoteDatabaseName)
        :C4ReplicatorImpl(db, params)
        ,_url(effectiveURL(serverAddress, remoteDatabaseName))
        ,_retryTimer(std::bind(&C4RemoteReplicator::retry, this, false))
        {
            if (params.socketFactory) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _customSocketFactory = *params.socketFactory;
                _socketFactory = &_customSocketFactory;
            }
        }


        void start(bool reset) noexcept override {
            LOCK(_mutex);
            if (_replicator)
                return;
            _retryCount = 0;
            if(!_restart(reset)) {
                UNLOCK();
                notifyStateChanged();
            }
        }


        virtual bool retry(bool resetCount) override {
            LOCK(_mutex);
            if (resetCount)
                _retryCount = 0;
            if (_status.level >= kC4Connecting)
                return true;
            if (_status.level == kC4Stopped)
                C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported, "Replicator is stopped");
            logInfo("Retrying connection to %.*s (attempt #%u)...", SPLAT(_url), _retryCount+1);
            if(!_restart(false)) {
                UNLOCK();
                notifyStateChanged();
                return false;
            }
            return true;
        }


        virtual void stop() noexcept override {
            cancelScheduledRetry();
            C4ReplicatorImpl::stop();
        }


        // Called by the client when it determines the remote host is [un]reachable.
        virtual void setHostReachable(bool reachable) noexcept override {
            LOCK(_mutex);
            if (!setStatusFlag(kC4HostReachable, reachable))
                return;
            logInfo("Notified that server is now %sreachable", (reachable ? "" : "un"));
            if (reachable)
                maybeScheduleRetry();
            else
                cancelScheduledRetry();
        }


        virtual void _suspend() noexcept override {
            // called with _mutex locked
            cancelScheduledRetry();
            C4ReplicatorImpl::_suspend();
        }


        virtual bool _unsuspend() noexcept override {
            // called with _mutex locked
            maybeScheduleRetry();
            return true;
        }


    protected:
        virtual alloc_slice URL() const noexcept override {
            return _url;
        }


        virtual void createReplicator() override {
            auto webSocket = CreateWebSocket(_url, socketOptions(), _database, _socketFactory);
            _replicator = new Replicator(_database->openAgain().get(), webSocket, *this, _options);
            
            // Yes this line is disgusting, but the memory addresses that the logger logs
            // are not the _actual_ addresses of the object, but rather the pointer to
            // its Logging virtual table since inside of _logVerbose this is all that
            // is known.
            _logVerbose("C4RemoteRepl %p created Repl %p", (Logging *)this, (Logging *)_replicator.get());
        }


        // Both `start` and `retry` end up calling this.
        bool _restart(bool reset) noexcept {
            cancelScheduledRetry();
            return _start(reset);
        }


        void maybeScheduleRetry() noexcept {
            if (_status.level == kC4Offline &&  statusFlag(kC4HostReachable)
                                            && !statusFlag(kC4Suspended)) {
                _retryCount = 0;
                scheduleRetry(0);
            }
        }


        // Starts the timer to call `retry` in the future.
        void scheduleRetry(unsigned delayInSecs) noexcept {
            _retryTimer.fireAfter(std::chrono::seconds(delayInSecs));
            setStatusFlag(kC4WillRetry, true);
        }


        // Cancels a previous call to `scheduleRetry`.
        void cancelScheduledRetry() noexcept {
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
            bool transient = c4err.mayBeTransient();
            if (transient || (continuous() && c4err.mayBeNetworkDependent())) {
                if (_retryCount >= maxRetryCount()) {
                    logError("Will not retry; max retry count (%u) reached", _retryCount);
                    return;
                }

                // OK, we are going offline, to retry later:
                _status.level = kC4Offline;

                string desc = c4err.description();
                if (transient || statusFlag(kC4HostReachable)) {
                    // On transient error, retry periodically, with exponential backoff:
                    unsigned delay = retryDelay(++_retryCount);
                    logError("Transient error (%s); attempt #%u in %u sec...",
                             desc.c_str(), _retryCount+1, delay);
                    scheduleRetry(delay);
                } else {
                    // On other network error, don't retry automatically. The client should await
                    // a network change and call c4repl_retry.
                    logError("Network error (%s); will retry when host becomes reachable...",
                             desc.c_str());
                }
            }
        }


        // The function governing the exponential backoff of retries
        unsigned retryDelay(unsigned retryCount) const noexcept {
            unsigned delay = 1 << std::min(retryCount, 30u);
            unsigned maxDelay = getIntProperty(kC4ReplicatorOptionMaxRetryInterval,
                                               kDefaultMaxRetryDelay);
            return std::min(delay, maxDelay);
        }


        // Returns the maximum number of (failed) retry attempts.
        unsigned maxRetryCount() const noexcept {
            unsigned defaultCount = continuous() ? kMaxContinuousRetryCount : kMaxOneShotRetryCount;
            return getIntProperty(kC4ReplicatorOptionMaxRetries, defaultCount);
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
            Replicator::Options opts(kC4Disabled, kC4Disabled, _options.properties);
            opts.setProperty(kC4SocketOptionWSProtocols, Replicator::ProtocolName().c_str());
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
