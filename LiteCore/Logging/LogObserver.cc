//
// LogObserver.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LogObserver.hh"
#include "LogFiles.hh"
#include "LogObjectMap.hh"
#include "Logging_Internal.hh"
#include "Error.hh"
#include "SmallVector.hh"
#include <compare>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <vector>
#include "betterassert.hh"

namespace litecore {
    using namespace std;
    using namespace litecore::loginternal;

    // Creates a formatted message from a RawLogEntry with its format string and va_list.
    __printflike(2, 0) static string formatEntry(RawLogEntry const& entry, const char* format, va_list args) {
        static mutex sFormatMutex;
        static char  sFormatBuffer[2048];
        unique_lock  lock(sFormatMutex);

        size_t len = 0;
        if ( entry.objRef != LogObjectRef::None ) {
            // Write the object (`Logging` instance) description:
            len = sObjectMap.addObjectPath(sFormatBuffer, sizeof(sFormatBuffer), entry.objRef);
        }
        if ( !entry.prefix.empty() ) {
            // Add the prefix string (created from Logger::addLoggingKeyValuePairs):
            Assert(len < sizeof(sFormatBuffer));
            size_t prefixLen = std::min(sizeof(sFormatBuffer) - len, entry.prefix.size());
            memcpy(&sFormatBuffer[len], entry.prefix.data(), prefixLen);
            len += prefixLen;
            if ( len < sizeof(sFormatBuffer) ) sFormatBuffer[len++] = ' ';
        }
        // Then format the printf args:
        len += vsnprintf(&sFormatBuffer[len], sizeof(sFormatBuffer) - len, format, args);
        return string(sFormatBuffer, len);
    }

#pragma mark - LOG OBSERVERS:

    // List of the observers of all domains
    static LogObservers* sDomainlessObservers;

    LogObservers::LogObservers() {
        if ( sDomainlessObservers ) _observers = sDomainlessObservers->_observers;
    }

    bool LogObservers::addObserver(LogObserver* obs, LogLevel level) {
        Assert(level >= LogLevel::Debug && level <= LogLevel::Error);
        unique_lock lock(_mutex);
        for ( auto& [o, lv] : _observers )
            if ( o == obs ) return false;
        auto i = _observers.begin();
        while ( i != _observers.end() && i->second < level ) ++i;
        _observers.insert(i, {obs, level}) + 1;
        return true;
    }

    bool LogObservers::removeObserver(LogObserver* obs) noexcept {
        unique_lock lock(_mutex);
        return std::erase_if(_observers, [obs](auto& o) { return o.first == obs; }) > 0;
    }

    LogLevel LogObservers::lowestLevel() const noexcept {
        unique_lock lock(_mutex);
        return _observers.empty() ? LogLevel::None : _observers.front().second;
    }

    void LogObservers::notify(RawLogEntry const& entry, const char* format, va_list args) {
        fleece::smallVector<fleece::Retained<LogObserver>, 4> curObservers;
        {
            // Temporarily lock, to copy the list of observers that will be notified:
            unique_lock lock(_mutex);
            for ( auto& [obs, obsLevel] : _observers ) {
                if ( obsLevel > entry.level ) break;
                if ( !(entry.fileOnly && !dynamic_cast<LogFiles*>(obs.get())) ) curObservers.push_back(obs);
            }
            if ( curObservers.empty() ) return;
        }

        string             formattedMessage;
        optional<LogEntry> formattedEntry;
        for ( auto& obs : curObservers ) {
            // A `va_list` is like a stream, so we have to copy it and let each callback read from a copy:
            va_list argsCopy;
            va_copy(argsCopy, args);
            if ( obs->raw() ) {
                obs->observe(entry, format, argsCopy);
            } else {
                if ( !formattedEntry ) {
                    formattedMessage = formatEntry(entry, format, argsCopy);
                    formattedEntry.emplace(
                            LogEntry{.domain = entry.domain, .level = entry.level, .message = formattedMessage});
                }
                obs->observe(*formattedEntry);
            }
        }
    }

    void LogObservers::notifyCallbacksOnly(LogEntry const& entry) {
        unique_lock lock(_mutex);
        for ( auto& [obs, obsLevel] : _observers ) {
            if ( obsLevel > entry.level ) break;
            if ( !obs->raw() && !dynamic_cast<LogFiles*>(obs.get()) ) obs->observe(entry);
        }
    }

#pragma mark - LOG OBSERVER:

    bool LogObserver::_addTo(LogDomain& domain, LogLevel level) {
        if ( level == LogLevel::None ) return true;
        if ( !domain._observers->addObserver(this, level) ) return false;
        domain.invalidateLevel();
        return true;
    }

    void LogObserver::_removeFrom(LogDomain& domain) {
        if ( domain._observers->removeObserver(this) ) domain.invalidateLevel();
    }

    void LogObserver::add(LogObserver* observer, LogLevel defaultLevel, span<const pair<LogDomain&, LogLevel>> levels) {
        for ( auto& dl : levels ) {
            if ( !observer->_addTo(dl.first, dl.second) )
                error::_throw(error::InvalidParameter, "LogObserver is already registered");
        }
        if ( defaultLevel != LogLevel::None ) {
            static once_flag sOnce;
            call_once(sOnce, [] { sDomainlessObservers = new LogObservers; });
            if ( !sDomainlessObservers->addObserver(observer, defaultLevel) )
                error::_throw(error::InvalidParameter, "LogObserver is already registered");
            for ( auto domain = LogDomain::first(); domain; domain = domain->next() )
                (void)observer->_addTo(*domain, defaultLevel);
        }
    }

    void LogObserver::remove(LogObserver* observer) {
        for ( auto domain = LogDomain::first(); domain; domain = domain->next() ) observer->_removeFrom(*domain);
        if ( sDomainlessObservers ) sDomainlessObservers->removeObserver(observer);
    }

    void LogObserver::observe(LogEntry const&) noexcept { assert("Should have been overridden" == nullptr); }

    void LogObserver::observe(RawLogEntry const&, const char*, va_list) noexcept {
        assert("Should have been overridden" == nullptr);
    }

}  // namespace litecore
