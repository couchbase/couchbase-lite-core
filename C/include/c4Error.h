//
// c4Error.h
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
#include "fleece/FLSlice.h"

#ifdef __cplusplus
#    include "fleece/slice.hh"
#    include <exception>
#    include <string>
#    include <cstdarg>
#    include <cstdint>
#else
#    include <stdarg.h>
#    include <stdint.h>
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Errors  Error Codes and Error Handling
    @{ */


// (These are identical to the internal C++ error::Domain enum values.)
typedef C4_ENUM(uint8_t, C4ErrorDomain){
        LiteCoreDomain = 1,  // code is a Couchbase Lite Core error code (see below)
        POSIXDomain,         // code is an errno
        SQLiteDomain,        // code is a SQLite error; see "sqlite3.h"
        FleeceDomain,        // code is a Fleece error; see "FleeceException.h"
        NetworkDomain,       // code is a network error; see enum C4NetworkErrorCode, below
        WebSocketDomain,     // code is a WebSocket close code (1000...1015) or HTTP error (300..599)
        MbedTLSDomain,       // code is an mbedTLS error; see "mbedtls/error.h"

        kC4MaxErrorDomainPlus1};


// LiteCoreDomain error codes:
// (These are identical to the internal C++ error::LiteCoreError enum values.)
// clang-format off
typedef C4_ENUM(int32_t, C4ErrorCode){
                             kC4ErrorAssertionFailed = 1,    // Internal assertion failure
                             kC4ErrorUnimplemented,          // Oops, an unimplemented API call
                             kC4ErrorUnsupportedEncryption,  // Unsupported encryption algorithm
                             kC4ErrorBadRevisionID,          // Invalid revision ID syntax
                             kC4ErrorCorruptRevisionData,    // Revision contains corrupted/unreadable data
                             kC4ErrorNotOpen,                // Database/KeyStore/index is not open
                             kC4ErrorNotFound,               // Document not found
                             kC4ErrorConflict,               // Document update conflict
                             kC4ErrorInvalidParameter,       // Invalid function parameter or struct value
                             kC4ErrorUnexpectedError,
                             /*10*/                         // Internal unexpected C++ exception
                             kC4ErrorCantOpenFile,          // Database file can't be opened; may not exist
                             kC4ErrorIOError,               // File I/O error
                             kC4ErrorMemoryError,           // Memory allocation failed (out of memory?)
                             kC4ErrorNotWriteable,          // File is not writeable
                             kC4ErrorCorruptData,           // Data is corrupted
                             kC4ErrorBusy,                  // Database is busy/locked
                             kC4ErrorNotInTransaction,      // Function must be called while in a transaction
                             kC4ErrorTransactionNotClosed,  // Database can't be closed while a transaction is open
                             kC4ErrorUnsupported,           // Operation not supported in this database
                             kC4ErrorNotADatabaseFile,
                             /*20*/                      // File is not a database, or encryption key is wrong
                             kC4ErrorWrongFormat,        // Database exists but not in the format/storage requested
                             kC4ErrorCrypto,             // Encryption/decryption error
                             kC4ErrorInvalidQuery,       // Invalid query
                             kC4ErrorMissingIndex,       // No such index, or query requires a nonexistent index
                             kC4ErrorInvalidQueryParam,  // Unknown query param name, or param number out of range
                             kC4ErrorRemoteError,        // Unknown error from remote server
                             kC4ErrorDatabaseTooOld,     // Database file format is older than what I can open
                             kC4ErrorDatabaseTooNew,     // Database file format is newer than what I can open
                             kC4ErrorBadDocID,           // Invalid document ID
                             kC4ErrorCantUpgradeDatabase,
                             /*30*/                     // DB can't be upgraded (might be unsupported dev version)
                             kC4ErrorDeltaBaseUnknown,  // Replicator can't apply delta: base revision body is missing
                             kC4ErrorCorruptDelta,      // Replicator can't apply delta: delta data invalid
                             kC4NumErrorCodesPlus1};
// clang-format on

/** Network error codes (potentially higher level than POSIX, lower level than HTTP.) 
    The entries marked with a POSIX code mirror that code so that platform bindings have
    a stable cross platform error code to use for transient or network dependent errors, and
    will behave the same as if the errno in question was passed.  Entries marked as retryable
    will cause a retry loop according to the configured retry rules.  */
// (These are identical to the internal C++ NetworkError enum values in WebSocketInterface.hh.)
typedef C4_ENUM(int32_t, C4NetworkErrorCode){
        kC4NetErrDNSFailure = 1,         // DNS lookup failed [retryable]
        kC4NetErrUnknownHost,            // DNS server doesn't know the hostname [retryable]
        kC4NetErrTimeout,                // Connection timeout [ETIMEDOUT, retryable]
        kC4NetErrInvalidURL,             // Invalid URL
        kC4NetErrTooManyRedirects,       // HTTP redirect loop
        kC4NetErrTLSHandshakeFailed,     // TLS handshake failed, for reasons other than below
        kC4NetErrTLSCertExpired,         // Peer's cert has expired
        kC4NetErrTLSCertUntrusted,       // Peer's cert isn't trusted for other reason
        kC4NetErrTLSCertRequiredByPeer,  // Peer (server) requires me to provide a (client) cert
        kC4NetErrTLSCertRejectedByPeer,  // Peer says my cert is invalid or unauthorized
        kC4NetErrTLSCertUnknownRoot,     // Self-signed cert, or unknown anchor cert
        kC4NetErrInvalidRedirect,        // Attempted redirect to invalid replication endpoint
        kC4NetErrUnknown,                // Unknown error
        kC4NetErrTLSCertRevoked,         // Peer's cert has been revoked
        kC4NetErrTLSCertNameMismatch,    // Peer's cert's Common Name doesn't match hostname
        kC4NetErrNetworkReset,           // The network subsystem was reset [ENETRESET, retryable]
        kC4NetErrConnectionAborted,      // The connection was aborted by the OS [ECONNABORTED, retryable]
        kC4NetErrConnectionReset,        // The connection was reset by the other side [ECONNRESET, retryable]
        kC4NetErrConnectionRefused,      // The other side refused the connection [ECONNREFUSED, retryable]
        kC4NetErrNetworkDown,            // The network subsystem is not functioning [ENETDOWN, retryable]
        kC4NetErrNetworkUnreachable,     // There is no usable network at the moment [ENETUNREACH, retryable]
        kC4NetErrNotConnected,           // The socket in question is no longer connected [ENOTCONN, retryable]
        kC4NetErrHostDown,               // The other side reports it is down [EHOSTDOWN, retryable]
        kC4NetErrHostUnreachable,        // There is no network path to the host [EHOSTUNREACH, retryable]
        kC4NetErrAddressNotAvailable,    // The address in question is already being used [EADDRNOTAVAIL, retryable]
        kC4NetErrBrokenPipe,             // Broken pipe [EPIPE, retryable]
        kC4NetErrUnknownInterface,       // The specified network interface is not valid or unknown.
        kC4NumNetErrorCodesPlus1};

/** An error value. These are returned by reference from API calls whose last parameter is a
    C4Error*. The semantics are based on Cocoa's usage of NSError:
    * A caller can pass NULL if it doesn't care about the error.
    * The error is filled in only if the function fails, as indicated by its return value
      (e.g. false or NULL.) If the function doesn't fail, it does NOT zero out the error, so its
      contents should be considered uninitialized garbage. */
typedef struct C4Error {
    C4ErrorDomain domain;         // Domain of error (LiteCore, POSIX, SQLite, ...)
    int           code;           // Error code. Domain-specific, except 0 is ALWAYS "none".
    unsigned      internal_info;  // No user-serviceable parts inside. Do not touch.

#ifdef __cplusplus
    // C4Error C++ API:
    NODISCARD static C4Error make(C4ErrorDomain, int code, fleece::slice message = {});
    NODISCARD static C4Error printf(C4ErrorDomain, int code, const char* format, ...) __printflike(3, 4);
    NODISCARD static C4Error vprintf(C4ErrorDomain, int code, const char* format, va_list args) __printflike(3, 0);
    static void set(C4Error* C4NULLABLE, C4ErrorDomain, int code, const char* format = nullptr, ...) __printflike(4, 5);

    static void set(C4ErrorDomain domain, int code, fleece::slice message, C4Error* C4NULLABLE outError) {
        if ( outError ) *outError = make(domain, code, message);
    }

    NODISCARD static C4Error fromException(const std::exception& e) noexcept;
    NODISCARD static C4Error fromCurrentException() noexcept;

    static void fromException(const std::exception& e, C4Error* C4NULLABLE outError) noexcept {
        if ( outError ) *outError = fromException(e);
    }

    static void fromCurrentException(C4Error* C4NULLABLE outError) noexcept {
        if ( outError ) *outError = fromCurrentException();
    }

    static void warnCurrentException(const char* inFunction) noexcept;

    [[noreturn]] static void raise(C4ErrorDomain, int code, const char* C4NULLABLE format = nullptr, ...)
            __printflike(3, 4);

    [[noreturn]] static void raise(C4Error e) { e.raise(); }

    static void setCaptureBacktraces(bool) noexcept;
    static bool getCaptureBacktraces() noexcept;

    [[noreturn]] void raise() const;

    bool operator==(const C4Error& b) const { return code == b.code && (code == 0 || domain == b.domain); }

    bool operator!=(const C4Error& b) const { return !(*this == b); }

    explicit operator bool() const { return code != 0; }

    bool operator!() const { return code == 0; }

    [[nodiscard]] std::string message() const;
    [[nodiscard]] std::string description() const;
    [[nodiscard]] std::string backtrace() const;

    [[nodiscard]] bool mayBeTransient() const noexcept;
    [[nodiscard]] bool mayBeNetworkDependent() const noexcept;
#endif
} C4Error;

#ifdef __cplusplus
static constexpr C4Error kC4NoError = {};
#else
#    define kC4NoError ((C4Error){})
#endif

// C4Error C API:


/** Returns an error message describing a C4Error. Remember to free the result. */
CBL_CORE_API FLStringResult c4error_getMessage(C4Error error) C4API;

/** Returns a description of an error, including the domain and code as well as the message.
    Remember to free the result. */
CBL_CORE_API FLSliceResult c4error_getDescription(C4Error error) C4API;

/** Returns a description of an error, including the domain and code as well as the message.
    The description is copied to the buffer as a C string.
    It will not write past the end of the buffer; the message will be truncated if necessary.
    @param error  The error to describe
    @param outBuffer  Where to write the C string to
    @param bufferSize  The size of the buffer
    @return  A pointer to the string, i.e. to the first byte of the buffer. */
CBL_CORE_API char* c4error_getDescriptionC(C4Error error, char* outBuffer, size_t bufferSize) C4API;

/** If set to `true`, then when a C4Error is created the current thread's stack backtrace will
    be captured along with it, and can later be examined by calling \ref c4error_getBacktrace.
    Even if false, some errors (like assertion failures) will still have backtraces. */
CBL_CORE_API void c4error_setCaptureBacktraces(bool) C4API;

CBL_CORE_API bool c4error_getCaptureBacktraces(void) C4API;

/** Returns the stack backtrace, if any, associated with a C4Error.
    This is formatted in human-readable form similar to a debugger or crash log. */
CBL_CORE_API FLStringResult c4error_getBacktrace(C4Error error) C4API;


/** Creates a C4Error struct with the given domain and code, and associates the message with it. */
NODISCARD CBL_CORE_API C4Error c4error_make(C4ErrorDomain domain, int code, FLString message) C4API;

/** Creates a C4Error struct with the given domain and code, formats the message as with
    `printf`, and associates the message with the error. */
NODISCARD CBL_CORE_API C4Error c4error_printf(C4ErrorDomain domain, int code, const char* format, ...) C4API
        __printflike(3, 4);

/** Same as \ref c4error_printf, but with a premade `va_list`. */
NODISCARD CBL_CORE_API C4Error c4error_vprintf(C4ErrorDomain domain, int code, const char* format, va_list args) C4API
        __printflike(3, 0);

/** Creates and stores a C4Error in `*outError`, if not NULL. Useful in functions that use the
    LiteCore error reporting convention of taking a `C4Error *outError` parameter. */
CBL_CORE_API void c4error_return(C4ErrorDomain domain, int code, FLString message, C4Error* C4NULLABLE outError) C4API;


/** Returns true if this is a network error that may be transient,
    i.e. the client should retry after a delay. */
CBL_CORE_API bool c4error_mayBeTransient(C4Error err) C4API;

/** Returns true if this error might go away when the network environment changes,
    i.e. the client should retry after notification of a network status change. */
CBL_CORE_API bool c4error_mayBeNetworkDependent(C4Error err) C4API;


/** @} */


C4API_END_DECLS
C4_ASSUME_NONNULL_END
