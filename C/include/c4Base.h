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
#include "c4Compat.h"
#include "fleece/FLSlice.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif


/** A database sequence number, representing the order in which a revision was created. */
typedef uint64_t C4SequenceNumber;


/** A date/time representation used for document expiration (and in date/time queries.)
    Measured in milliseconds since the Unix epoch (1/1/1970, midnight UTC.) */
typedef int64_t C4Timestamp;


/** Returns the current time, in milliseconds since 1/1/1970. */
C4Timestamp c4_now(void) C4API;


typedef struct {
    void* pointer;
    void (*destructor)(void*);
} C4ExtraInfo;


//////// SLICES:

// (This is just renaming stuff from FLSlice.h)

typedef FLSlice C4Slice;
typedef FLHeapSlice C4HeapSlice;
typedef FLSliceResult C4SliceResult;
typedef C4Slice C4String;
typedef C4HeapSlice C4HeapString;
typedef C4SliceResult C4StringResult;

static C4INLINE C4Slice c4str(const char *str) {return FLStr(str);}
#define C4STR(STR) FLSTR(STR)
#define kC4SliceNull kFLSliceNull

static inline bool c4SliceEqual(C4Slice a, C4Slice b)       {return FLSlice_Equal(a,b);}
static inline void c4slice_free(C4SliceResult s)            {FLSliceResult_Release(s);}


//////// COMMON TYPES

/** A raw SHA-1 digest used as the unique identifier of a blob. */
typedef struct C4BlobKey {
    uint8_t bytes[20];
} C4BlobKey;


/** A simple parsed-URL type. */
typedef struct C4Address C4Address;

/** Opaque handle for an object that manages storage of blobs. */
typedef struct c4BlobStore C4BlobStore;

/** An X.509 certificate, or certificate signing request (CSR). */
typedef struct C4Cert C4Cert;

/** Opaque handle to an opened database. */
typedef struct c4Database C4Database;

/** A database-observer reference. */
typedef struct c4DatabaseObserver C4DatabaseObserver;

/** Describes a version-controlled document. */
typedef struct C4Document C4Document;

/** A document-observer reference. */
typedef struct c4DocumentObserver C4DocumentObserver;

/** Opaque handle to a document enumerator. */
typedef struct C4DocEnumerator C4DocEnumerator;

/** An asymmetric key or key-pair (RSA, etc.) The private key may or may not be present. */
typedef struct C4KeyPair C4KeyPair;

/** A LiteCore network listener -- supports the REST API, replication, or both. */
typedef struct C4Listener C4Listener;

/** Opaque handle to a compiled query. */
typedef struct c4Query C4Query;

/** A query result enumerator. */
typedef struct C4QueryEnumerator C4QueryEnumerator;

/** A query-observer reference. */
typedef struct c4QueryObserver C4QueryObserver;

/** Contents of a raw document. */
typedef struct C4RawDocument C4RawDocument;

/** An open stream for reading data from a blob. */
typedef struct c4ReadStream C4ReadStream;

/** Opaque reference to a replicator. */
typedef struct C4Replicator C4Replicator;

/** Represents an open bidirectional stream of bytes or messages (typically a TCP socket.) */
typedef struct C4Socket C4Socket;

/** A group of callbacks that define the implementation of sockets. */
typedef struct C4SocketFactory C4SocketFactory;

/** An open stream for writing data to a blob. */
typedef struct c4WriteStream C4WriteStream;


//////// REFERENCE COUNTING


// The actual functions behind c4xxx_retain / c4xxx_release; don't call directly
void* c4base_retain(void *obj) C4API;
void c4base_release(void *obj) C4API;

// These types are reference counted and have c4xxx_retain / c4xxx_release functions:
static inline C4Cert* c4cert_retain(C4Cert* r) C4API {return (C4Cert*)c4base_retain(r);}
static inline void    c4cert_release(C4Cert* r) C4API {c4base_release(r);}
static inline C4KeyPair* c4keypair_retain(C4KeyPair* r) C4API {return (C4KeyPair*)c4base_retain(r);}
static inline void       c4keypair_release(C4KeyPair* r) C4API {c4base_release(r);}
static inline C4Database* c4db_retain(C4Database* r) C4API {return (C4Database*)c4base_retain(r);}
static inline void        c4db_release(C4Database* r) C4API {c4base_release(r);}
static inline C4Query* c4query_retain(C4Query* r) C4API {return (C4Query*)c4base_retain(r);}
static inline void     c4query_release(C4Query* r) C4API {c4base_release(r);}
    
C4Document* c4doc_retain(C4Document* r) C4API;
void        c4doc_release(C4Document* r) C4API;
C4QueryEnumerator* c4queryenum_retain(C4QueryEnumerator* r) C4API;
void               c4queryenum_release(C4QueryEnumerator* r) C4API;

// These types are _not_ ref-counted but must be freed after use:
void c4dbobs_free   (C4DatabaseObserver*) C4API;
void c4docobs_free  (C4DocumentObserver*) C4API;
void c4enum_free    (C4DocEnumerator*) C4API;
void c4listener_free(C4Listener*) C4API;
void c4queryobs_free(C4QueryObserver*) C4API;
void c4raw_free     (C4RawDocument*) C4API;
void c4repl_free    (C4Replicator*) C4API;
void c4stream_close(C4ReadStream*) C4API;
void c4stream_closeWriter(C4WriteStream*) C4API;


/** Returns the number of objects that have been created but not yet freed.
    This can be used as a debugging/testing tool to detect leaks. */
int c4_getObjectCount(void) C4API;

void c4_dumpInstances(void) C4API;


//////// ERRORS:


// (These are identical to the internal C++ error::Domain enum values.)
typedef C4_ENUM(uint32_t, C4ErrorDomain) {
    LiteCoreDomain = 1, // code is a Couchbase Lite Core error code (see below)
    POSIXDomain,        // code is an errno
    SQLiteDomain,       // code is a SQLite error; see "sqlite3.h"
    FleeceDomain,       // code is a Fleece error; see "FleeceException.h"
    NetworkDomain,      // code is a network error; see enum C4NetworkErrorCode, below
    WebSocketDomain,    // code is a WebSocket close code (1000...1015) or HTTP error (300..599)
    MbedTLSDomain,      // code is an mbedTLS error; see "mbedtls/error.h"

    kC4MaxErrorDomainPlus1
};


// LiteCoreDomain error codes:
// (These are identical to the internal C++ error::LiteCoreError enum values.)
typedef C4_ENUM(int32_t, C4ErrorCode) {
    kC4ErrorAssertionFailed = 1,    // Internal assertion failure
    kC4ErrorUnimplemented,          // Oops, an unimplemented API call
    kC4ErrorUnsupportedEncryption,  // Unsupported encryption algorithm
    kC4ErrorBadRevisionID,          // Invalid revision ID syntax
    kC4ErrorCorruptRevisionData,    // Revision contains corrupted/unreadable data
    kC4ErrorNotOpen,                // Database/KeyStore/index is not open
    kC4ErrorNotFound,               // Document not found
    kC4ErrorConflict,               // Document update conflict
    kC4ErrorInvalidParameter,       // Invalid function parameter or struct value
    kC4ErrorUnexpectedError, /*10*/ // Internal unexpected C++ exception
    kC4ErrorCantOpenFile,           // Database file can't be opened; may not exist
    kC4ErrorIOError,                // File I/O error
    kC4ErrorMemoryError,            // Memory allocation failed (out of memory?)
    kC4ErrorNotWriteable,           // File is not writeable
    kC4ErrorCorruptData,            // Data is corrupted
    kC4ErrorBusy,                   // Database is busy/locked
    kC4ErrorNotInTransaction,       // Function must be called while in a transaction
    kC4ErrorTransactionNotClosed,   // Database can't be closed while a transaction is open
    kC4ErrorUnsupported,            // Operation not supported in this database
    kC4ErrorNotADatabaseFile,/*20*/ // File is not a database, or encryption key is wrong
    kC4ErrorWrongFormat,            // Database exists but not in the format/storage requested
    kC4ErrorCrypto,                 // Encryption/decryption error
    kC4ErrorInvalidQuery,           // Invalid query
    kC4ErrorMissingIndex,           // No such index, or query requires a nonexistent index
    kC4ErrorInvalidQueryParam,      // Unknown query param name, or param number out of range
    kC4ErrorRemoteError,            // Unknown error from remote server
    kC4ErrorDatabaseTooOld,         // Database file format is older than what I can open
    kC4ErrorDatabaseTooNew,         // Database file format is newer than what I can open
    kC4ErrorBadDocID,               // Invalid document ID
    kC4ErrorCantUpgradeDatabase,/*30*/ // DB can't be upgraded (might be unsupported dev version)
    kC4ErrorDeltaBaseUnknown,       // Replicator can't apply delta: base revision body is missing
    kC4ErrorCorruptDelta,           // Replicator can't apply delta: delta data invalid
    kC4NumErrorCodesPlus1
};


/** Network error codes (higher level than POSIX, lower level than HTTP.) */
// (These are identical to the internal C++ NetworkError enum values in WebSocketInterface.hh.)
typedef C4_ENUM(int32_t, C4NetworkErrorCode) {
    kC4NetErrDNSFailure = 1,            // DNS lookup failed
    kC4NetErrUnknownHost,               // DNS server doesn't know the hostname
    kC4NetErrTimeout,                   // Connection timeout
    kC4NetErrInvalidURL,                // Invalid URL
    kC4NetErrTooManyRedirects,          // HTTP redirect loop
    kC4NetErrTLSHandshakeFailed,        // TLS handshake failed, for reasons other than below
    kC4NetErrTLSCertExpired,            // Peer's cert has expired
    kC4NetErrTLSCertUntrusted,          // Peer's cert isn't trusted for other reason
    kC4NetErrTLSCertRequiredByPeer,     // Peer (server) requires me to provide a (client) cert
    kC4NetErrTLSCertRejectedByPeer,     // Peer says my cert is invalid or unauthorized
    kC4NetErrTLSCertUnknownRoot,        // Self-signed cert, or unknown anchor cert
    kC4NetErrInvalidRedirect,           // Attempted redirect to invalid replication endpoint
    kC4NetErrUnknown,                   // Unknown error
    kC4NetErrTLSCertRevoked,            // Peer's cert has been revoked
    kC4NetErrTLSCertNameMismatch,       // Peer's cert's Common Name doesn't match hostname
    kC4NumNetErrorCodesPlus1
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


/** Returns an error message describing a C4Error. Remember to free the result. */
C4StringResult c4error_getMessage(C4Error error) C4API;

/** Returns a description of an error, including the domain and code as well as the message.
    Remember to free the result. */
C4SliceResult c4error_getDescription(C4Error error) C4API;

/** Returns a description of an error, including the domain and code as well as the message.
    The description is copied to the buffer as a C string.
    It will not write past the end of the buffer; the message will be truncated if necessary.
    @param error  The error to describe
    @param buffer  Where to write the C string to
    @param bufferSize  The size of the buffer
    @return  A pointer to the string, i.e. to the first byte of the buffer. */
char* c4error_getDescriptionC(C4Error error, char buffer[] C4NONNULL, size_t bufferSize) C4API;

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

    typedef struct {
        C4LogLevel log_level;       ///< The log level that the overall logging will limit to
        C4String base_path;         ///< The path to the binary log file base name (other elements will be added)
        int64_t max_size_bytes;     ///< The maximum size of each log file (minimum 1024)
        int32_t max_rotate_count;   ///< The maximum amount of old log files to keep
        bool use_plaintext;         ///< Disables binary encoding of the logs (not recommended)
        C4String header;            ///< Header to print at the start of every log file
    } C4LogFileOptions;

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
    @param options The options to use when setting up the binary logger
    @param error  On failure, the filesystem error that caused the call to fail.
    @return  True on success, false on failure. */
bool c4log_writeToBinaryFile(C4LogFileOptions options, C4Error *error) C4API;

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

/** Returns true if logging to this domain at this level will have an effect.
    This is called by the C4Log macros (below), to skip the possibly-expensive evaluation of
    parameters if nothing will be logged anyway.
    (This is not the same as comparing c4log_getLevel, because even if the domain's level
    indicates it would log, logging could still be suppressed by the global callbackLevel or
    binaryFileLevel.) */
bool c4log_willLog(C4LogDomain C4NONNULL, C4LogLevel) C4API;

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
        do {if (c4log_willLog(DOMAIN, LEVEL))   \
            c4log(DOMAIN, LEVEL, FMT, ## __VA_ARGS__);} while (false)
#define C4Debug(FMT, ...)           C4LogToAt(kC4DefaultLog, kC4LogDebug,   FMT, ## __VA_ARGS__)
#define C4Log(FMT, ...)             C4LogToAt(kC4DefaultLog, kC4LogInfo,    FMT, ## __VA_ARGS__)
#define C4LogVerbose(FMT, ...)      C4LogToAt(kC4DefaultLog, kC4LogVerbose, FMT, ## __VA_ARGS__)
#define C4Warn(FMT, ...)            C4LogToAt(kC4DefaultLog, kC4LogWarning, FMT, ## __VA_ARGS__)
#define C4WarnError(FMT, ...)       C4LogToAt(kC4DefaultLog, kC4LogError,   FMT, ## __VA_ARGS__)


//////// INFO:


/** A string describing the version of LiteCore. Currently this just describes the Git branch and
    commit, in the form "Built from master branch, commit 0bc68f0d". */
C4StringResult c4_getBuildInfo(void) C4API;

/** A short version string. */
C4StringResult c4_getVersion(void) C4API;


//////// MISCELLANEOUS:


/** Specifies a directory to use for temporary files. You don't normally need to call this,
    unless you're on a platform where it's impossible to reliably discover the location of the
    system temporary directory (i.e. Android), or you have some other good reason to want temp
    files stored elsewhere.
    @note  If you do call this function, you should call it before opening any databases.
    @note  Needless to say, the directory must already exist. */
void c4_setTempDir(C4String path) C4API;

/** Schedules a function to be called asynchronously on a background thread.
    @param task  A pointer to the function to run. It must take a single `void*` argument and
        return `void`. If it needs to return a value, it should call some other function you
        define and pass that value as a parameter.
    @param context  An arbitrary pointer that will be passed to the function. You can use this
        to provide state. Obviously, whatever this points to must remain valid until the
        future time when `task` is called. */
void c4_runAsyncTask(void (*task)(void*) C4NONNULL, void *context) C4API;

#ifdef __cplusplus
}
#endif
