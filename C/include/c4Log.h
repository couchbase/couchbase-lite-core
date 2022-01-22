//
// c4Log.h
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Compat.h"
#include "c4Error.h"
#include "fleece/FLSlice.h"
#include <stdarg.h>

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup  Logging  Logging
    @{ */


/** Logging levels. */
typedef C4_ENUM(int8_t, C4LogLevel) {
    kC4LogDebug,    /// Super-verbose messages that are only enabled in debug builds of LiteCore
    kC4LogVerbose,  /// More info than you normally want
    kC4LogInfo,     /// Informational messages
    kC4LogWarning,  /// Warnings about something unusual that might be a problem
    kC4LogError,    /// Errors that occur; these might be handled internally
    kC4LogNone      /// Setting this level will disable logging entirely
};

/** Reference to a _log domain_: a specific source of logs that can be enabled or disabled. */
typedef struct c4LogDomain *C4LogDomain;


/** Subsystems that produce logs. Log levels can be configured for each, so you can focus your
    diagnostic efforts on the area of interest. */
CBL_CORE_API extern const C4LogDomain
    kC4DefaultLog,                  ///< The default log domain
    kC4DatabaseLog,                 ///< Log domain for database operations
    kC4QueryLog,                    ///< Log domain for query operations
    kC4SyncLog,                     ///< Log domain for replication operations
    kC4WebSocketLog;                ///< Log domain for WebSocket operations


#pragma mark - FILE LOGGING:


/** Configuration for file-based logging. */
typedef struct C4LogFileOptions {
    C4LogLevel log_level;        ///< The minimum level of message to be logged
    FLString   base_path;        ///< The path to the binary log file base name (other elements will be added)
    int64_t    max_size_bytes;   ///< The maximum size of each log file (minimum 1024)
    int32_t    max_rotate_count; ///< The maximum amount of old log files to keep
    bool       use_plaintext;    ///< Disables binary encoding of the logs (not recommended)
    FLString   header;           ///< Header text to print at the start of every log file
} C4LogFileOptions;

/** Causes log messages to be written to a file, overwriting any previous contents.
    The data is written in an efficient and compact binary form that can be read using the
    "litecorelog" tool.
    @param options The options to use when setting up the binary logger
    @param error  On failure, the filesystem error that caused the call to fail.
    @return  True on success, false on failure. */
bool c4log_writeToBinaryFile(C4LogFileOptions options, C4Error* C4NULLABLE error) C4API;

/** Returns the filesystem path of the directory where log files are kept. */
FLStringResult c4log_binaryFilePath(void) C4API;

/** Ensures all log messages have been written to the current log files. */
void c4log_flushLogFiles(void) C4API;

/** Returns the minimum level of log messages to be written to the log file,
    regardless of what level individual log domains are set to. */
C4LogLevel c4log_binaryFileLevel(void) C4API;

/** Sets the minimum level of log messages to be written to the log file. */
void c4log_setBinaryFileLevel(C4LogLevel level) C4API;


#pragma mark - CALLBACK LOGGING:


/** A logging callback that the application can register. */
typedef void (* C4NULLABLE C4LogCallback)(C4LogDomain, C4LogLevel, const char *fmt, va_list args);

/** Registers (or unregisters) a log callback, and sets the minimum log level to report.
    Before this is called, a default callback is used that writes to stderr at the Info level.
    NOTE: this setting is global to the entire process.
    @param level  The minimum level of message to log.
    @param callback  The logging callback, or NULL to disable logging entirely.
    @param preformatted  If true, log messages will be formatted before invoking the callback,
            so the `fmt` parameter will be the actual string to log, and the `args` parameter
            will be NULL. */
void c4log_writeToCallback(C4LogLevel level, C4LogCallback callback, bool preformatted) C4API;

/** Returns the current logging callback, or the default one if none has been set. */
C4LogCallback c4log_getCallback(void) C4API;

/** Returns the minimum level of log messages to be reported via callback,
    regardless of what level individual log domains are set to. */
C4LogLevel c4log_callbackLevel(void) C4API;

/** Sets the minimum level of log messages to be reported via callback. */
void c4log_setCallbackLevel(C4LogLevel level) C4API;


#pragma mark - LOG DOMAINS:


/** Looks up a named log domain.
    @param name  The name of the domain, or NULL for the default domain.
    @param create  If true, the domain will be created if it doesn't exist.
    @return  The domain object, or NULL if not found. */
C4LogDomain c4log_getDomain(const char *name, bool create) C4API;

/** Returns the name of a log domain. (The default domain's name is an empty string.) */
const char* c4log_getDomainName(C4LogDomain) C4API;

/** Returns the current log level of a domain, the minimum level of message it will log. */
C4LogLevel c4log_getLevel(C4LogDomain) C4API;

/** Returns true if logging to this domain at this level will have an effect.
    This is called by the C4Log macros (below), to skip the possibly-expensive evaluation of
    parameters if nothing will be logged anyway.
    (This is not the same as comparing c4log_getLevel, because even if the domain's level
    indicates it would log, logging could still be suppressed by the global callbackLevel or
    binaryFileLevel.) */
bool c4log_willLog(C4LogDomain, C4LogLevel) C4API;

/** Changes the level of the given log domain.
    This setting is global to the entire process.
    Logging is further limited by the levels assigned to the current callback and/or binary file.
    For example, if you set the Foo domain's level to Verbose, and the current log callback is
    at level Warning while the binary file is at Verbose, then verbose Foo log messages will be
    written to the file but not to the callback. */
void c4log_setLevel(C4LogDomain c4Domain, C4LogLevel level) C4API;


#pragma mark - LOGGING EXCEPTIONs:


/** If set to true, LiteCore will log a warning of the form "LiteCore throwing %s error %d: %s"
    just before throwing an internal exception. This can be a good way to catch the source where
    an error occurs. */
void c4log_warnOnErrors(bool) C4API;

/** Returns true if warn-on-errors is on; see \ref c4log_warnOnErrors. Default is false.*/
bool c4log_getWarnOnErrors(void) C4API;

/** Registers a handler with the C++ runtime that will log a backtrace when an uncaught C++
    exception occurs, just before the process aborts. */
void c4log_enableFatalExceptionBacktrace(void) C4API;


#pragma mark - WRITING LOG MESSAGES:


/** Logs a message/warning/error to a specific domain, if its current level is less than
    or equal to the given level. This message will then be written to the current callback and/or
    binary file, if their levels are less than or equal to the given level.
    @param domain  The domain to log to.
    @param level  The level of the message. If the domain's level is greater than this,
                    nothing will be logged.
    @param fmt  printf-style format string, followed by arguments (if any). */
void c4log(C4LogDomain domain, C4LogLevel level, const char *fmt, ...) C4API __printflike(3,4);

/** Same as c4log, for use in calling functions that already take variable args. */
void c4vlog(C4LogDomain domain, C4LogLevel level, const char *fmt, va_list args) C4API
    __printflike(3, 0);

/** Same as c4log, except it accepts preformatted messages as FLSlices */
void c4slog(C4LogDomain domain, C4LogLevel level, FLString msg) C4API;

// Convenient aliases for c4log:
#define C4LogToAt(DOMAIN, LEVEL, FMT, ...)        \
        do {if (c4log_willLog(DOMAIN, LEVEL))   \
            c4log(DOMAIN, LEVEL, FMT, ## __VA_ARGS__);} while (false)
#define C4Debug(FMT, ...)           C4LogToAt(kC4DefaultLog, kC4LogDebug,   FMT, ## __VA_ARGS__)
#define C4Log(FMT, ...)             C4LogToAt(kC4DefaultLog, kC4LogInfo,    FMT, ## __VA_ARGS__)
#define C4LogVerbose(FMT, ...)      C4LogToAt(kC4DefaultLog, kC4LogVerbose, FMT, ## __VA_ARGS__)
#define C4Warn(FMT, ...)            C4LogToAt(kC4DefaultLog, kC4LogWarning, FMT, ## __VA_ARGS__)
#define C4WarnError(FMT, ...)       C4LogToAt(kC4DefaultLog, kC4LogError,   FMT, ## __VA_ARGS__)


/** @} */


C4API_END_DECLS
C4_ASSUME_NONNULL_END
