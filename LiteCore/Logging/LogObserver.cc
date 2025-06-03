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

    static constexpr size_t kMessageBufferSize = 252;

    // Creates a formatted message from a RawLogEntry with its format string and va_list.
    __printflike(2, 0) static alloc_slice formatEntry(RawLogEntry const& entry, const char* format, va_list args) {
        alloc_slice message(kMessageBufferSize);
        auto        buf = (char*)message.buf;
        size_t      len = 0;
        if ( entry.objRef != LogObjectRef::None ) {
            // Write the object (`Logging` instance) description:
            len = sObjectMap.addObjectPath(buf, kMessageBufferSize, entry.objRef);
        }
        if ( !entry.prefix.empty() ) {
            // Add the prefix string (created from Logger::addLoggingKeyValuePairs):
            Assert(len < kMessageBufferSize);
            size_t prefixLen = std::min(kMessageBufferSize - len, entry.prefix.size());
            memcpy(&buf[len], entry.prefix.data(), prefixLen);
            len += prefixLen;
            if ( len < kMessageBufferSize ) buf[len++] = ' ';
        }

        // Then format the printf args:
        va_list argsCopy;
        va_copy(argsCopy, args);
        auto written = vsnprintf(&buf[len], kMessageBufferSize - len, format, args);
        if ( len + written >= kMessageBufferSize ) {
            // Grow the alloc_slice if necessary. `written` is the number of bytes vsnprintf
            // wants to write, without the trailing null.
            message.resize(len + written + 1);
            buf     = (char*)message.buf;
            written = vsnprintf(&buf[len], message.size - len, format, argsCopy);
            assert(len + written < message.size);
        }
        va_end(argsCopy);
        message.shorten(len + written);  // just changes `size` to match the message length
        return message;
    }

    LogEntry::LogEntry(uint64_t t, LogDomain& d, LogLevel lv, fleece::slice msg)
        : timestamp(t), domain(d), level(lv), message(alloc_slice::nullPaddedString(msg)) {}

    LogEntry::LogEntry(RawLogEntry const& raw, const char* format, va_list args)
        : timestamp(raw.timestamp), domain(raw.domain), level(raw.level), message(formatEntry(raw, format, args)) {}

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
        _observers.insert(i, {obs, level});
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

        optional<LogEntry> formattedEntry;
        for ( auto& obs : curObservers ) {
            // A `va_list` is like a stream, so we have to copy it and let each callback read from a copy:
            va_list argsCopy;
            va_copy(argsCopy, args);
            if ( obs->raw() ) {
                obs->observe(entry, format, argsCopy);
            } else {
                if ( !formattedEntry ) formattedEntry.emplace(entry, format, argsCopy);
                obs->observe(*formattedEntry);
            }
            va_end(argsCopy);
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
