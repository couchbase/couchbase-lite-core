//
// c4Base.h
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#ifndef __has_extension
#define __has_extension(x) 0
#endif

#ifdef _MSC_VER
#define C4INLINE __forceinline
#define C4NONNULL
#else
#define C4INLINE inline
#define C4NONNULL __attribute((nonnull))
#endif

// Macros for defining typed enumerations and option flags.
// To define an enumeration whose values won't be combined:
//      typedef C4_ENUM(baseIntType, name) { ... };
// To define an enumeration of option flags that will be ORed together:
//      typedef C4_OPTIONS(baseIntType, name) { ... };
// These aren't just a convenience; they are required for Swift bindings.
#if APPLE
    #include <CoreFoundation/CFBase.h>      /* for CF_ENUM and CF_OPTIONS macros */
    #define C4_ENUM CF_ENUM
    #define C4_OPTIONS CF_OPTIONS
#elif DOXYGEN_PARSING
    #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
    #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
#else
    #if (__cplusplus && _MSC_VER) || (__cplusplus && __cplusplus >= 201103L && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum))) || (!__cplusplus && __has_feature(objc_fixed_enum))
        #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
        #if (__cplusplus)
            #define C4_OPTIONS(_type, _name) _type _name; enum : _type
        #else
            #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
        #endif
    #else
        #define C4_ENUM(_type, _name) _type _name; enum
        #define C4_OPTIONS(_type, _name) _type _name; enum
    #endif
#endif


#ifdef __cplusplus
#define C4API noexcept
#else
#define C4API
#endif


#ifdef __cplusplus
extern "C" {
#endif

// Export/import stuff:
#ifdef _MSC_VER
#ifdef LITECORE_EXPORTS
#define CBL_CORE_API __declspec(dllexport)
#else
#define CBL_CORE_API __declspec(dllimport)
#endif
#else // _MSC_VER
#define CBL_CORE_API
#endif

// Type-checking for printf-style vararg functions:
#ifdef _MSC_VER
#define __printflike(A, B)
#else
#ifndef __printflike
#define __printflike(fmtarg, firstvararg) __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#endif
#endif



/** A database sequence number, representing the order in which a revision was created. */
typedef uint64_t C4SequenceNumber;


//////// ERRORS:


// (These are identical to the internal C++ error::Domain enum values.)
typedef C4_ENUM(uint32_t, C4ErrorDomain) {
    LiteCoreDomain = 1, // code is a Couchbase Lite Core error code (see below)
    POSIXDomain,        // code is an errno
                        // domain 3 is unused
    SQLiteDomain = 4,   // code is a SQLite error
    FleeceDomain,       // code is a Fleece error
    NetworkDomain,      // code is a network error code from the enum below
    WebSocketDomain,    // code is a WebSocket close code (1000...1015) or HTTP error (400..599)

    kC4MaxErrorDomainPlus1
};


// LiteCoreDomain error codes:
// (These are identical to the internal C++ error::LiteCoreError enum values.)
typedef C4_ENUM(int32_t, C4ErrorCode) {
    kC4ErrorAssertionFailed = 1,    // Internal assertion failure
    kC4ErrorUnimplemented,          // Oops, an unimplemented API call
    kC4ErrorNoSequences,            // This KeyStore does not support sequences
    kC4ErrorUnsupportedEncryption,  // Unsupported encryption algorithm
    kC4ErrorNoTransaction,          // Function must be called within a transaction
    kC4ErrorBadRevisionID,          // Invalid revision ID syntax
    kC4ErrorBadVersionVector,       // Invalid version vector syntax
    kC4ErrorCorruptRevisionData,    // Revision contains corrupted/unreadable data
    kC4ErrorCorruptIndexData,       // Index contains corrupted/unreadable data
    kC4ErrorTokenizerError, /*10*/  // can't create text tokenizer for FTS
    kC4ErrorNotOpen,                // Database/KeyStore/index is not open
    kC4ErrorNotFound,               // Document not found
    kC4ErrorDeleted,                // Document has been deleted
    kC4ErrorConflict,               // Document update conflict
    kC4ErrorInvalidParameter,       // Invalid function parameter or struct value
    kC4ErrorDatabaseError,          // Lower-level database error (ForestDB or SQLite)
    kC4ErrorUnexpectedError,        // Internal unexpected C++ exception
    kC4ErrorCantOpenFile,           // Database file can't be opened; may not exist
    kC4ErrorIOError,                // File I/O error
    kC4ErrorCommitFailed, /*20*/    // Transaction commit failed
    kC4ErrorMemoryError,            // Memory allocation failed (out of memory?)
    kC4ErrorNotWriteable,           // File is not writeable
    kC4ErrorCorruptData,            // Data is corrupted
    kC4ErrorBusy,                   // Database is busy/locked
    kC4ErrorNotInTransaction,       // Function cannot be called while in a transaction
    kC4ErrorTransactionNotClosed,   // Database can't be closed while a transaction is open
    kC4ErrorIndexBusy,              // (unused)
    kC4ErrorUnsupported,            // Operation not supported in this database
    kC4ErrorNotADatabaseFile,       // File is not a database, or encryption key is wrong
    kC4ErrorWrongFormat, /*30*/     // Database exists but not in the format/storage requested
    kC4ErrorCrypto,                 // Encryption/decryption error
    kC4ErrorInvalidQuery,           // Invalid query
    kC4ErrorMissingIndex,           // No such index, or query requires a nonexistent index
    kC4ErrorInvalidQueryParam,      // Unknown query param name, or param number out of range
    kC4ErrorRemoteError,            // Unknown error from remote server
    kC4ErrorDatabaseTooOld,         // Database file format is older than what I can open
    kC4ErrorDatabaseTooNew,         // Database file format is newer than what I can open
    kC4ErrorBadDocID,               // Invalid document ID
    kC4ErrorCantUpgradeDatabase,    // Database can't be upgraded (might be unsupported dev version)

    kC4NumErrorCodesPlus1
};


/** Network error codes (higher level than POSIX, lower level than HTTP.) */
// (These are identical to the internal C++ NetworkError enum values in WebSocketInterface.hh.)
typedef C4_ENUM(int32_t, C4NetworkErrorCode) {
    kC4NetErrDNSFailure = 1,             // DNS lookup failed
    kC4NetErrUnknownHost,                // DNS server doesn't know the hostname
    kC4NetErrTimeout,
    kC4NetErrInvalidURL,
    kC4NetErrTooManyRedirects,
    kC4NetErrTLSHandshakeFailed,
    kC4NetErrTLSCertExpired,
    kC4NetErrTLSCertUntrusted,              // Cert isn't trusted for other reason
    kC4NetErrTLSClientCertRequired,
    kC4NetErrTLSClientCertRejected, // 10
    kC4NetErrTLSCertUnknownRoot,            // Self-signed cert, or unknown anchor cert
};


/** An error value. These are returned by reference from API calls whose last parameter is a
    C4Error*. The semantics are based on Cocoa's usage of NSError:
    * A caller can pass NULL if it doesn't care about the error.
    * The error is filled in only if the function fails, as indicated by its return value
      (e.g. false or NULL.) If the function doesn't fail, it does NOT zero out the error, so its
      contents should be considered uninitialized garbage. */
typedef struct {
    C4ErrorDomain domain;
    int32_t code;
    int32_t internal_info;
} C4Error;


//////// SLICES:


#ifndef C4_IMPL

#ifdef FL_SLICE_DEFINED
    typedef FLSlice C4Slice;
    typedef FLSliceResult C4SliceResult;
#else
    /** A slice is simply a pointer to a range of bytes, usually interpreted as a UTF-8 string.
        A "null slice" has chars==NULL and length==0.
        A slice with length==0 is not necessarily null; if chars!=NULL it's an empty string.
        A slice as a function parameter is temporary and read-only: the function will not alter or free
        the bytes, and the pointer won't be accessed after the function returns.
        A slice _returned from_ a function points to newly-allocated memory and must be freed by the
        caller, with c4slice_free(). */
    typedef struct {
        const void *buf;
        size_t size;
    } C4Slice;

    /** Denotes a slice returned from a function, which needs to have its buf freed by the caller. */
    typedef C4Slice C4SliceResult;
#endif

#endif // C4_IMPL
    
    
typedef C4Slice C4String;
typedef C4SliceResult C4StringResult;

/** Creates a slice pointing to the contents of a C string. */
static C4INLINE C4Slice c4str(const char *str) {
    C4Slice foo = { str, str ? strlen(str) : 0 };
    return foo;
}

// Macro version of c4str, for use in initializing compile-time constants.
// STR must be a C string literal.
#ifdef _MSC_VER
#define C4STR(STR) C4Slice({("" STR), sizeof(("" STR))-1})
#else
#define C4STR(STR) ((C4Slice){("" STR), sizeof(("" STR))-1})
#endif

// A convenient constant denoting a null slice.

#if !defined(kC4SliceNull)
#ifdef _MSC_VER
static const C4Slice kC4SliceNull = { NULL, 0 };
#else
#define kC4SliceNull ((C4Slice){NULL, 0})
#endif
#endif

/** Returns true if two slices have equal contents. */
bool c4SliceEqual(C4Slice a, C4Slice b) C4API;

/** Frees the memory of a heap-allocated slice by calling free(buf). */
void c4slice_free(C4SliceResult) C4API;


//////// ERRORS:


/** Returns an error message describing a C4Error. Remember to free the result. */
C4StringResult c4error_getMessage(C4Error error) C4API;

/** Writes an error message describing a C4Error to a buffer, as a C string.
    It will not write past the end of the buffer; the message will be truncated if necessary.
    @param error  The error to describe
    @param buffer  Where to write the C string to
    @param bufferSize  The size of the buffer
    @return  A pointer to the string, i.e. to the first byte of the buffer. */
char* c4error_getMessageC(C4Error error, char buffer[] C4NONNULL, size_t bufferSize) C4API;

/** Creates a C4Error struct with the given domain and code, and associates the message with it. */
C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) C4API;


/** Returns true if this is a network error that may be transient,
    i.e. the client should retry after a delay. */
bool c4error_mayBeTransient(C4Error err) C4API;

/** Returns true if this error might go away when the network environment changes,
    i.e. the client should retry after notification of a network status change. */
bool c4error_mayBeNetworkDependent(C4Error err) C4API;


//////// LOGGING:


/** Logging levels. */
typedef C4_ENUM(int8_t, C4LogLevel) {
    kC4LogDebug,
    kC4LogVerbose,
    kC4LogInfo,
    kC4LogWarning,
    kC4LogError,
    kC4LogNone
};

/** A log domain, a specific source of logs that can be enabled or disabled. */
typedef struct c4LogDomain *C4LogDomain;

/** A logging callback that the application can register. */
typedef void (*C4LogCallback)(C4LogDomain, C4LogLevel, const char *fmt C4NONNULL, va_list args);


CBL_CORE_API extern const C4LogDomain
    kC4DefaultLog,                  ///< The default log domain
    kC4DatabaseLog,                 ///< Log domain for database operations
    kC4QueryLog,                    ///< Log domain for query operations
    kC4SyncLog,                     ///< Log domain for replication operations
    kC4WebSocketLog;                ///< Log domain for WebSocket operations


/** Registers (or unregisters) a log callback, and sets the minimum log level to report.
    Before this is called, a default callback is used that writes to stderr at the Info level.
    NOTE: this setting is global to the entire process.
    @param level  The minimum level of message to log.
    @param callback  The logging callback, or NULL to disable logging entirely.
    @param preformatted  If true, log messages will be formatted before invoking the callback,
            so the `fmt` parameter will be the actual string to log, and the `args` parameter
            will be NULL. */
void c4log_writeToCallback(C4LogLevel level, C4LogCallback callback, bool preformatted) C4API;

/** Causes log messages to be written to a file, overwriting any previous contents.
    The data is written in an efficient and compact binary form that can be read using the
    "litecorelog" tool.
    @param level  The minimum level of message to log.
    @param path  The filesystem path of the file to write to, or a NULL slice for none.
    @param error  On failure, the filesystem error that caused the call to fail.
    @return  True on success, false on failure. */
bool c4log_writeToBinaryFile(C4LogLevel level, C4String path, C4Error *error) C4API;

C4LogLevel c4log_callbackLevel(void) C4API;
void c4log_setCallbackLevel(C4LogLevel level) C4API;

C4LogLevel c4log_binaryFileLevel(void) C4API;
void c4log_setBinaryFileLevel(C4LogLevel level) C4API;

/** Looks up a named log domain.
    @param name  The name of the domain, or NULL for the default domain.
    @param create  If true, the domain will be created if it doesn't exist.
    @return  The domain object, or NULL if not found. */
C4LogDomain c4log_getDomain(const char *name, bool create) C4API;

/** Returns the name of a log domain. (The default domain's name is an empty string.) */
const char* c4log_getDomainName(C4LogDomain C4NONNULL) C4API;

/** Returns the current log level of a domain, the minimum level of message it will log. */
C4LogLevel c4log_getLevel(C4LogDomain C4NONNULL) C4API;

/** Changes the level of the given log domain.
    This setting is global to the entire process.
    Logging is further limited by the levels assigned to the current callback and/or binary file.
    For example, if you set the Foo domain's level to Verbose, and the current log callback is
    at level Warning while the binary file is at Verbose, then verbose Foo log messages will be
    written to the file but not to the callback. */
void c4log_setLevel(C4LogDomain c4Domain C4NONNULL, C4LogLevel level) C4API;

/** Logs a message/warning/error to a specific domain, if its current level is less than
    or equal to the given level. This message will then be written to the current callback and/or
    binary file, if their levels are less than or equal to the given level.
    @param domain  The domain to log to.
    @param level  The level of the message. If the domain's level is greater than this,
                    nothing will be logged.
    @param fmt  printf-style format string, followed by arguments (if any). */
void c4log(C4LogDomain domain C4NONNULL, C4LogLevel level, const char *fmt C4NONNULL, ...) C4API __printflike(3,4);

/** Same as c4log, for use in calling functions that already take variable args. */
void c4vlog(C4LogDomain domain C4NONNULL, C4LogLevel level, const char *fmt C4NONNULL, va_list args) C4API;

/** Same as c4log, except it accepts preformatted messages as C4Slices */
void c4slog(C4LogDomain domain C4NONNULL, C4LogLevel level, C4String msg) C4API;

// Convenient aliases for c4log:
#define C4LogToAt(DOMAIN, LEVEL, FMT, ...)        \
        {if (c4log_getLevel(DOMAIN) <= LEVEL)   \
            c4log(DOMAIN, LEVEL, FMT, ## __VA_ARGS__);}
#define C4Debug(FMT, ...)           C4LogToAt(kC4DefaultLog, kC4LogDebug,   FMT, ## __VA_ARGS__)
#define C4Log(FMT, ...)             C4LogToAt(kC4DefaultLog, kC4LogInfo,    FMT, ## __VA_ARGS__)
#define C4Warn(FMT, ...)            C4LogToAt(kC4DefaultLog, kC4LogWarning, FMT, ## __VA_ARGS__)
#define C4WarnError(FMT, ...)       C4LogToAt(kC4DefaultLog, kC4LogError,   FMT, ## __VA_ARGS__)


//////// INFO:


/** A string describing the version of LiteCore. Currently this just describes the Git branch and
    commit, in the form "Built from master branch, commit 0bc68f0d". */
C4StringResult c4_getBuildInfo(void) C4API;

/** A short version string. */
C4StringResult c4_getVersion(void) C4API;


/** Returns the number of objects that have been created but not yet freed.
    This can be used as a debugging/testing tool to detect leaks. */
int c4_getObjectCount(void) C4API;

void c4_dumpInstances(void) C4API;


#ifdef __cplusplus
}
#endif
