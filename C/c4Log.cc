//
// c4Log.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Log.h"
#include "Backtrace.hh"
#include "Error.hh"
#include "LogFiles.hh"
#include "LogFunction.hh"
#include "LogObserver.hh"
#include "Logging.hh"
#include "PlatformIO.hh"  // For vasprintf on Windows
#include "WebSocketInterface.hh"
#include "c4ExceptionUtils.hh"

using namespace std;
using namespace litecore;

// NOLINTBEGIN(cppcoreguidelines-interfaces-global-init)
CBL_CORE_API const C4LogDomain kC4DefaultLog   = (C4LogDomain)&kC4Cpp_DefaultLog;
CBL_CORE_API const C4LogDomain kC4DatabaseLog  = (C4LogDomain)&DBLog;
CBL_CORE_API const C4LogDomain kC4QueryLog     = (C4LogDomain)&QueryLog;
CBL_CORE_API const C4LogDomain kC4SyncLog      = (C4LogDomain)&SyncLog;
CBL_CORE_API const C4LogDomain kC4WebSocketLog = (C4LogDomain)&websocket::WSLogDomain;

// NOLINTEND(cppcoreguidelines-interfaces-global-init)

namespace litecore {

    /** A LogObserver that calls a C4LogObserverCallback or a C4LogCallback. */
    class LogCallback : public LogObserver {
      public:
        LogCallback(C4LogObserverCallback C4NULLABLE callback, void* C4NULLABLE context)
            : _obsCallback(callback), _context(context) {}

        LogCallback(C4LogCallback C4NULLABLE legacyCallback, bool preformatted)
            : _legacyCallback(legacyCallback), _preformatted(preformatted) {}

      private:
        void observe(LogEntry const& e) noexcept override {
            if ( _obsCallback ) {
                C4LogEntry c4entry{.timestamp = C4Timestamp(e.timestamp),
                                   .level     = C4LogLevel(e.level),
                                   .domain    = C4LogDomain(&e.domain),
                                   .message   = e.message};
                _obsCallback(&c4entry, _context);
            } else if ( _preformatted ) {
                va_list dummy_args{};
                _legacyCallback(C4LogDomain(&e.domain), C4LogLevel(e.level), e.messageStr(), dummy_args);
            } else {
                // The legacy callback wants a format string and va_list, but we're given a preformatted string.
                // (The reason we don't ask for a 'raw' callback is that using it requires formatting
                // the objRef, which is complicated. Best to let LogObserver's internal `formatEntry`
                // do it for us.)
                // To get a valid va_list for the callback, call a function that takes variable args:
                observeVA(e, "%s", e.message.data());  // yes, e.message is nul-terminated
            }
        }

        void observeVA(LogEntry const& e, const char* fmt, ...) {
            // `fmt` will always be "%s" and the next arg will be the formatted log message.
            va_list args;
            va_start(args, fmt);
            _legacyCallback(C4LogDomain(&e.domain), C4LogLevel(e.level), fmt, args);
            va_end(args);
        }

        C4LogObserverCallback const _obsCallback    = nullptr;
        C4LogCallback const         _legacyCallback = nullptr;
        void* const                 _context        = nullptr;
        bool                        _preformatted   = true;
    };

}  // namespace litecore

static inline LogDomain* toInternal(C4LogDomain d) { return reinterpret_cast<LogDomain*>(d); }

static inline C4LogDomain toExternal(LogDomain* d) { return reinterpret_cast<C4LogDomain>(d); }

#pragma mark - LOG OBSERVER:

// There is no real `C4LogObserver` struct. `C4LogObserver*` is an alias for C++ `LogObserver*`.

static inline LogObserver* toInternal(C4LogObserver* obs) { return reinterpret_cast<LogObserver*>(obs); }

static inline C4LogObserver* toExternal(Retained<LogObserver> obs) {
    return reinterpret_cast<C4LogObserver*>(std::move(obs).detach());
}

static vector<pair<LogDomain&, LogLevel>> convertDomains(C4LogObserverConfig const& config) {
    vector<pair<LogDomain&, LogLevel>> domains;
    domains.reserve(config.domainsCount);
    for ( size_t i = 0; i < config.domainsCount; ++i ) {
        auto [domain, level] = config.domains[i];
        if ( domain == nullptr || level < kC4LogDebug || level > kC4LogError )
            error::_throw(error::InvalidParameter, "invalid log domain or level");
        for ( size_t j = 0; j < i; j++ ) {
            if ( config.domains[j].domain == domain ) error::_throw(error::InvalidParameter, "duplicate log domain");
        }
        domains.emplace_back(*toInternal(domain), LogLevel(level));
    }
    return domains;
}

static LogFiles::Options convertFileOptions(C4LogFileOptions const& fopts) {
    LogFiles::Options options{.directory   = slice(fopts.base_path).asString(),
                              .maxSize     = fopts.max_size_bytes,
                              .maxCount    = fopts.max_rotate_count,
                              .isPlaintext = fopts.use_plaintext};
    if ( fopts.header ) options.initialMessage = slice(fopts.header).asString();
    else
        options.initialMessage = "Generated by LiteCore "s + string(c4_getBuildInfo());
    return options;
}

C4LogObserver* c4log_newObserver(C4LogObserverConfig config, C4Error* outError) noexcept {
    try {
        if ( (config.callback != nullptr) + (config.fileOptions != nullptr) != 1 ) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter,
                           "log observer needs either a callback or a file but not both"_sl, outError);
        }
        auto                  domains = convertDomains(config);
        Retained<LogObserver> obs;
        if ( config.callback ) obs = make_retained<LogCallback>(config.callback, config.callbackContext);
        else
            obs = make_retained<LogFiles>(convertFileOptions(*config.fileOptions));
        LogObserver::add(obs, LogLevel(config.defaultLevel), domains);
        return toExternal(std::move(obs));
    }
    catchError(outError);
    return nullptr;
}

void c4log_removeObserver(C4LogObserver* observer) noexcept {
    LogObserver::remove(reinterpret_cast<LogObserver*>(observer));
}

C4LogObserver* c4log_replaceObserver(C4LogObserver* oldObs, C4LogObserverConfig config, C4Error* outError) noexcept {
    try {
        Retained<LogFiles> fileObs = dynamic_cast<LogFiles*>(toInternal(oldObs));
        if ( fileObs && config.fileOptions ) {
            // If the old and new config both log to files, try to update the LogFiles' options.
            // That allows it to keep log files open.
            auto domains = convertDomains(config);
            if ( fileObs->setOptions(convertFileOptions(*config.fileOptions)) ) {
                LogObserver::remove(fileObs);
                LogObserver::add(fileObs, LogLevel(config.defaultLevel), domains);
                return toExternal(std::move(fileObs));
            }
        }
        // By default just create a new observer and remove the old one:
        C4LogObserver* newObs = c4log_newObserver(config, outError);
        if ( newObs && oldObs ) c4log_removeObserver(oldObs);
        return newObs;
    }
    catchError(outError);
    return nullptr;
}

void c4log_consoleObserverCallback(const C4LogEntry* entry, void* context) noexcept {
    LogFunction::logToConsole(
            LogEntry(uint64_t(entry->timestamp), *toInternal(entry->domain), LogLevel(entry->level), entry->message));
}

void c4logobserver_flush(C4LogObserver* obs) C4API {
    if ( auto logFiles = dynamic_cast<LogFiles*>(toInternal(obs)) ) {
        try {
            logFiles->flush();
        }
        catchAndWarn();
    }
}

#pragma mark - CALLBACK LOGGING:

static C4LogObserver* sDefaultLogCallback;
static C4LogCallback  sDefaultLogCallbackFn;
static C4LogLevel     sDefaultLogCallbackLevel = kC4LogNone;
static bool           sDefaultLogCallbackPreformatted;

// LCOV_EXCL_START
void c4log_writeToCallback(C4LogLevel level, C4LogCallback callback, bool preformatted) noexcept {
    if ( !callback ) level = kC4LogNone;
    if ( sDefaultLogCallback ) {
        c4log_removeObserver(sDefaultLogCallback);
        c4logobserver_release(sDefaultLogCallback);
        sDefaultLogCallback = nullptr;
    }
    if ( level != kC4LogNone ) {
        auto obs = make_retained<LogCallback>(callback, preformatted);
        LogObserver::add(obs, LogLevel(level));
        sDefaultLogCallback = toExternal(std::move(obs));
    }
    sDefaultLogCallbackLevel        = level;
    sDefaultLogCallbackPreformatted = preformatted;
}

C4LogCallback c4log_getCallback() noexcept { return sDefaultLogCallbackFn; }

C4LogLevel c4log_callbackLevel() noexcept { return sDefaultLogCallbackLevel; }  // LCOV_EXCL_LINE

void c4log_setCallbackLevel(C4LogLevel level) noexcept {
    if ( level != sDefaultLogCallbackLevel && sDefaultLogCallback )
        c4log_writeToCallback(level, sDefaultLogCallbackFn, sDefaultLogCallbackPreformatted);
    sDefaultLogCallbackLevel = level;
}

void c4log_initConsole(C4LogLevel level) noexcept {
    auto defaultCallback = [](C4LogDomain domain, C4LogLevel level, const char* message, va_list) {
        LogFunction::logToConsole(LogEntry(uint64_t(c4_now()), *toInternal(domain), LogLevel(level), message));
    };
    c4log_writeToCallback(level, defaultCallback, true);
}

// LCOV_EXCL_STOP

#pragma mark - FILE LOGGING:

static C4LogObserver* sDefaultLogFiles;
static C4LogLevel     sDefaultLogFilesLevel = kC4LogNone;

static void endFileLogging() {
    if ( sDefaultLogFiles ) {
        c4log_removeObserver(sDefaultLogFiles);
        c4logobserver_release(sDefaultLogFiles);
        sDefaultLogFiles = nullptr;
    }
}

bool c4log_writeToBinaryFile(C4LogFileOptions options, C4Error* outError) noexcept {
    if ( options.base_path.empty() || options.log_level == kC4LogNone ) {
        // Disabling file logging:
        endFileLogging();
        sDefaultLogFilesLevel = kC4LogNone;
    } else {
        C4LogObserverConfig config{.defaultLevel = options.log_level, .fileOptions = &options};
        auto                newObs = c4log_replaceObserver(sDefaultLogFiles, config, outError);
        if ( !newObs ) return false;
        c4logobserver_release(sDefaultLogFiles);
        sDefaultLogFiles      = newObs;
        sDefaultLogFilesLevel = options.log_level;

        static once_flag sOnce;
        call_once(sOnce, []() {
            atexit([] {
                // Make sure to flush the log file on exit:
                if ( auto logFile = sDefaultLogFiles ) {
                    c4logobserver_flush(logFile);
                    c4logobserver_release(logFile);
                }
            });
        });
    }
    return true;
}

C4LogLevel c4log_binaryFileLevel() noexcept { return sDefaultLogFilesLevel; }

void c4log_setBinaryFileLevel(C4LogLevel level) noexcept {
    if ( sDefaultLogFiles && level != sDefaultLogFilesLevel ) {
        if ( level == kC4LogNone ) {
            endFileLogging();
        } else {
            auto logFiles = dynamic_cast<LogFiles*>(toInternal(sDefaultLogFiles));
            LogObserver::remove(logFiles);
            LogObserver::add(logFiles, LogLevel(level));
        }
    }
    sDefaultLogFilesLevel = level;
}

C4StringResult c4log_binaryFilePath() C4API {
    if ( auto logFiles = dynamic_cast<LogFiles*>(toInternal(sDefaultLogFiles)) )
        return C4StringResult(alloc_slice(logFiles->options().directory));
    return {};
}

void c4log_flushLogFiles() C4API {
    if ( sDefaultLogFiles ) c4logobserver_flush(sDefaultLogFiles);
}

#pragma mark - LOG DOMAINS AND LEVELS:

C4LogDomain c4log_getDomain(const char* name, bool create) noexcept {
    if ( !name ) return kC4DefaultLog;
    auto domain = LogDomain::named(name);
    if ( !domain && create ) domain = new LogDomain(strdup(name));
    return toExternal(domain);
}

const char* c4log_getDomainName(C4LogDomain c4Domain) noexcept {
    auto domain = toInternal(c4Domain);
    return domain->name();
}

C4LogDomain c4log_nextDomain(C4LogDomain domain) noexcept {
    if ( domain ) return toExternal(toInternal(domain)->next());
    else
        return toExternal(LogDomain::first());
}

C4LogLevel c4log_getLevel(C4LogDomain c4Domain) noexcept {
    auto domain = toInternal(c4Domain);
    return (C4LogLevel)domain->effectiveLevel();
}

void c4log_setLevel(C4LogDomain c4Domain, C4LogLevel level) noexcept {
    auto domain = (LogDomain*)c4Domain;
    domain->setLevel((LogLevel)level);
}

bool c4log_willLog(C4LogDomain c4Domain, C4LogLevel level) C4API {
    auto domain = toInternal(c4Domain);
    return domain->willLog((LogLevel)level);
}

void c4log_warnOnErrors(bool warn) noexcept { error::sWarnOnError = warn; }

bool c4log_getWarnOnErrors() noexcept { return error::sWarnOnError; }

void c4log_enableFatalExceptionBacktrace() C4API {
    fleece::Backtrace::installTerminateHandler([](const string& backtrace) {
        c4log(kC4DefaultLog, kC4LogError,
              "FATAL ERROR (backtrace follows)\n"
              "********************\n"
              "%s\n"
              "******************** NOW TERMINATING",
              backtrace.c_str());
    });
}

#pragma mark - WRITING LOG MESSAGES:

void c4log(C4LogDomain c4Domain, C4LogLevel level, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    c4vlog(c4Domain, level, fmt, args);
    va_end(args);
}

void c4vlog(C4LogDomain c4Domain, C4LogLevel level, const char* fmt, va_list args) noexcept {
    try {
        toInternal(c4Domain)->vlog((LogLevel)level, fmt, args);
    } catch ( ... ) {}
}

// LCOV_EXCL_START
void c4slog(C4LogDomain c4Domain, C4LogLevel level, C4Slice msg) noexcept {
    if ( msg.buf == nullptr ) { return; }

    try {
        toInternal(c4Domain)->logNoCallback((LogLevel)level, "%.*s", FMTSLICE(msg));
    } catch ( ... ) {}
}

// LCOV_EXCL_STOP
