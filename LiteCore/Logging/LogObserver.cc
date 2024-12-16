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
#include "Logging_Internal.hh"
#include "Error.hh"
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


#pragma mark - LOG OBSERVERS:

    // List of the observers of all domains
    static LogObservers* sDomainlessObservers;

    LogObservers::LogObservers() {
        if ( sDomainlessObservers ) _observers = sDomainlessObservers->_observers;
    }

    bool LogObservers::addObserver(LogObserver* obs, LogLevel level) {
        Assert(level >= LogLevel::Debug && level <= LogLevel::Error);
        for ( auto& [o, lv] : _observers )
            if ( o == obs ) return false;
        auto i = _observers.begin();
        while ( i != _observers.end() && i->second < level ) ++i;
        _observers.insert(i, {obs, level}) + 1;
        return true;
    }

    bool LogObservers::removeObserver(LogObserver* obs) noexcept {
        return std::erase_if(_observers, [obs](auto& o) { return o.first == obs; }) > 0;
    }

    LogLevel LogObservers::lowestLevel() const noexcept {
        return _observers.empty() ? LogLevel::None : _observers.front().second;
    }

    void LogObservers::notify(RawLogEntry const& entry, const char* format, va_list args) {
        optional<LogEntry> formattedEntry;
        for ( auto& [obs, obsLevel] : _observers ) {
            if ( obsLevel > entry.level ) {
                break;
            } else if ( obs->raw() ) {
                obs->observe(entry, format, args);
            } else {
                if ( !formattedEntry ) {
                    int len = vsnprintf(sFormatBuffer, sizeof(sFormatBuffer), format, args);
                    formattedEntry.emplace(LogEntry{.domain  = entry.domain,
                                                    .level   = entry.level,
                                                    .message = string_view(sFormatBuffer, len)});
                }
                obs->observe(*formattedEntry);
            }
        }
    }

#pragma mark - LOG OBSERVER:

    void LogObserver::_addTo(LogDomain& domain, LogLevel level) {
        if ( level == LogLevel::None ) return;
        if ( !domain._observers->addObserver(this, level) )
            error::_throw(error::InvalidParameter, "LogObserver is already registered");
        domain._effectiveLevel = LogLevel::Uninitialized;
    }

    void LogObserver::_removeFrom(LogDomain& domain) {
        if ( domain._observers->removeObserver(this) ) domain._effectiveLevel = LogLevel::Uninitialized;
    }

    void LogObserver::add(LogObserver* observer, LogLevel defaultLevel, span<const pair<LogDomain&, LogLevel>> levels) {
        lock_guard lock(sLogMutex);
        _add(observer, defaultLevel, levels);
    }

    void LogObserver::_add(LogObserver* observer, LogLevel defaultLevel,
                           span<const pair<LogDomain&, LogLevel>> levels) {
        for ( auto& dl : levels ) observer->_addTo(dl.first, dl.second);
        if ( defaultLevel != LogLevel::None ) {
            if ( !sDomainlessObservers ) sDomainlessObservers = new LogObservers;
            if ( !sDomainlessObservers->addObserver(observer, defaultLevel) )
                error::_throw(error::InvalidParameter, "LogObserver is already registered");
            for ( auto domain = LogDomain::sFirstDomain; domain; domain = domain->_next )
                (void)observer->_addTo(*domain, defaultLevel);
        }
    }

    void LogObserver::remove(LogObserver* observer) {
        lock_guard lock(sLogMutex);
        _remove(observer);
    }

    void LogObserver::_remove(LogObserver* observer) {
        for ( LogDomain* domain = LogDomain::sFirstDomain; domain; domain = domain->_next )
            observer->_removeFrom(*domain);
        if ( sDomainlessObservers ) sDomainlessObservers->removeObserver(observer);
    }

    void LogObserver::observe(LogEntry const&) noexcept { assert("Should have been overridden" == nullptr); }

    void LogObserver::observe(RawLogEntry const&, const char*, va_list) noexcept {
        assert("Should have been overridden" == nullptr);
    }
}  // namespace litecore
