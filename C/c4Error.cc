//
// c4Error.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Base.h"
#include "c4Internal.hh"
#include "Backtrace.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "WebSocketInterface.hh"  // For WebSocket error codes
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>

using namespace std;
using namespace litecore;

using Backtrace = fleece::Backtrace;

namespace litecore {

    static_assert((int)kC4MaxErrorDomainPlus1 == (int)error::NumDomainsPlus1,
                  "C4 error domains are not in sync with C++ ones");
    static_assert((int)kC4NumErrorCodesPlus1 == (int)error::NumLiteCoreErrorsPlus1,
                  "C4 error codes are not in sync with C++ ones");
    static_assert((int)kC4NumNetErrorCodesPlus1 == (int)litecore::websocket::kNetErrorMaxPlus1,
                  "C4 net error codes are not in sync with C++ ones");


#pragma mark - ERROR INFO:

    /** Additional attributes of a recently-created C4Error. */
    struct ErrorInfo {
      public:
        string                message;    ///< The error message, if any
        shared_ptr<Backtrace> backtrace;  ///< The error's captured backtrace, if any
    };

    /** A singleton class whose instance stores ErrorInfo for C4Errors.
        The ErrorInfos are stored in an array. The C4Error's `internal_info`, if nonzero, gives the
        array index of its ErrorInfo. Old array items are discarded to limit the space used. */
    class ErrorTable {
      public:
        /// The singleton instance.
        static ErrorTable& instance() {
            static ErrorTable sInstance;
            return sInstance;
        }

        /// Core function that creates/initializes a C4Error.
        __cold C4Error makeError(C4ErrorDomain domain, int code, ErrorInfo info,
                                 unsigned skipStackFrames = 0) noexcept {
            C4Error error{domain, code, 0};
            if ( error::sCaptureBacktraces && !info.backtrace )
                info.backtrace = Backtrace::capture(skipStackFrames + 2);
            if ( !info.message.empty() || info.backtrace ) {
                lock_guard<mutex> lock(_mutex);
                try {
                    if ( _table.size() >= kMaxErrorMessagesToSave ) {
                        _table.pop_front();
                        ++_tableStart;
                    }
                    _table.emplace_back(std::move(info));
                    error.internal_info = (uint32_t)(_tableStart + _table.size() - 1);
                } catch ( ... ) {}
            }
            return error;
        }

        /// Creates a C4Error with a formatted message.
        __cold __printflike(4, 0) C4Error vmakeError(C4ErrorDomain domain, int code, const char* format, va_list args,
                                                     unsigned skipStackFrames = 0) noexcept {
            ErrorInfo info;
            if ( format && *format ) {
                try {
                    info.message = vstringprintf(format, args);
                } catch ( ... ) {}
            }
            return makeError(domain, code, std::move(info), skipStackFrames + 1);
        }

        /// Returns the ErrorInfo associated with a C4Error.
        /// (We have to return a copy; returning a reference would not be thread-safe.)
        __cold optional<ErrorInfo> copy(C4Error error) noexcept {
            if ( error.internal_info == 0 ) return nullopt;
            lock_guard<mutex> lock(_mutex);
            if ( int64_t(error.internal_info) - int64_t(_tableStart) < 0 )  // Ensure we won't escape range of uint32
                return nullopt;
            uint32_t tableIndex = error.internal_info - _tableStart;
            if ( tableIndex < _table.size() ) return _table[tableIndex];
            else
                return nullopt;
        }

      private:
        deque<ErrorInfo> _table;           ///< Stores ErrorInfo objects for C4Errors
        uint32_t         _tableStart = 1;  ///< internal_info of 1st item in sTable
        mutex            _mutex;           ///< mutex guarding the above
    };

}  // namespace litecore

__cold static const char* getErrorName(C4Error err) {
    static constexpr const char* kLiteCoreNames[] = {
            // These must match up with the codes in the declaration of LiteCoreError
            "no error",  // 0
            "AssertionFailed",
            "Unimplemented",
            "UnsupportedEncryption",
            "BadRevisionID",
            "CorruptRevisionData",
            "NotOpen",
            "NotFound",
            "Conflict",
            "InvalidParameter",
            "UnexpectedError",
            "CantOpenFile",
            "IOError",
            "MemoryError",
            "NotWriteable",
            "CorruptData",
            "Busy",
            "NotInTransaction",
            "TransactionNotClosed",
            "UnsupportedOperation",
            "NotADatabaseFile",
            "WrongFormat",
            "CryptoError",
            "InvalidQuery",
            "NoSuchIndex",
            "InvalidQueryParam",
            "RemoteError",
            "DatabaseTooOld",
            "DatabaseTooNew",
            "BadDocID",
            "CantUpgradeDatabase",
            "DeltaBaseUnknown",
            "CorruptDelta",
    };
    static_assert(sizeof(kLiteCoreNames) / sizeof(kLiteCoreNames[0]) == error::NumLiteCoreErrorsPlus1,
                  "Incomplete error message table");

    if ( err.domain == LiteCoreDomain && err.code < sizeof(kLiteCoreNames) / sizeof(char*) )
        return kLiteCoreNames[err.code];
    else
        return nullptr;
}

#pragma mark - PUBLIC API:

__cold C4Error C4Error::make(C4ErrorDomain domain, int code, slice message) {
    ErrorInfo info;
    if ( message.size > 0 ) info.message = string(slice(message));
    return ErrorTable::instance().makeError(domain, code, std::move(info));
}

__cold C4Error C4Error::vprintf(C4ErrorDomain domain, int code, const char* format, va_list args) {
    return ErrorTable::instance().vmakeError(domain, code, format, args);
}

__cold C4Error C4Error::printf(C4ErrorDomain domain, int code, const char* format, ...) {
    va_list args;
    va_start(args, format);
    auto err = vprintf(domain, code, format, args);
    va_end(args);
    return err;
}

__cold void C4Error::set(C4Error* outError, C4ErrorDomain domain, int code, const char* format, ...) {
    if ( outError ) {
        if ( format && format[0] ) {
            va_list args;
            va_start(args, format);
            *outError = vprintf(domain, code, format, args);
            va_end(args);
        } else {
            *outError = make(domain, code);
        }
    }
}

__cold C4Error C4Error::fromException(const exception& x) noexcept {
    error e = error::convertException(x).standardized();
    return ErrorTable::instance().makeError((C4ErrorDomain)e.domain, e.code, {e.what(), e.backtrace});
}

__cold C4Error C4Error::fromCurrentException() noexcept { return fromException(error::convertCurrentException()); }

static string getMessage(const C4Error& c4err) {
    auto info = ErrorTable::instance().copy(c4err);
    return info ? info->message : "";
}

namespace litecore {
    error::error(const C4Error& c4err) : error(error::Domain(c4err.domain), c4err.code, getMessage(c4err)) {
        if ( auto info = ErrorTable::instance().copy(c4err) ) backtrace = info->backtrace;
    }
}  // namespace litecore

[[noreturn]] __cold void C4Error::raise() const { throw litecore::error(*this); }

[[noreturn]] __cold void C4Error::raise(C4ErrorDomain domain, int code, const char* format, ...) {
    std::string message;
    if ( format ) {
        va_list args;
        va_start(args, format);
        message = vstringprintf(format, args);
        va_end(args);
    }
    error{error::Domain(domain), code, message}._throw(1);
}

__cold string C4Error::message() const {
    if ( code == 0 ) {
        return "";
    } else if ( domain < 1 || domain >= (C4ErrorDomain)error::NumDomainsPlus1 ) {
        return "invalid C4Error (unknown domain)";
    } else if ( auto info = ErrorTable::instance().copy(*this); info && !info->message.empty() ) {
        // Custom message referenced in info field
        return info->message;
    } else {
        // No; get the regular error message for this domain/code:
        return error((error::Domain)domain, code).what();
    }
}

__cold string C4Error::description() const {
    if ( code == 0 ) return "No error";
    const char*  errName = getErrorName(*this);
    stringstream str;
    str << error::nameOfDomain((error::Domain)domain) << " ";
    if ( errName ) str << errName;
    else
        str << "error " << code;
    str << ", \"" << message() << "\"";
    return str.str();
}

__cold string C4Error::backtrace() const {
    if ( auto info = ErrorTable::instance().copy(*this); info && info->backtrace ) return info->backtrace->toString();
    return "";
}

__cold bool C4Error::getCaptureBacktraces() noexcept { return error::sCaptureBacktraces; }

__cold void C4Error::setCaptureBacktraces(bool c) noexcept { error::sCaptureBacktraces = c; }

#pragma mark - PUBLIC C API:

__cold C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) noexcept {
    return C4Error::make(domain, code, message);
}

__cold C4Error c4error_printf(C4ErrorDomain domain, int code, const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    C4Error error = C4Error::vprintf(domain, code, format, args);
    va_end(args);
    return error;
}

__cold C4Error c4error_vprintf(C4ErrorDomain domain, int code, const char* format, va_list args) noexcept {
    return C4Error::vprintf(domain, code, format, args);
}

__cold void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error* outError) noexcept {
    if ( outError ) *outError = ErrorTable::instance().makeError(domain, code, {string(slice(message))});
}

__cold C4SliceResult c4error_getMessage(C4Error err) noexcept {
    if ( string msg = err.message(); msg.empty() ) return {};
    else
        return toSliceResult(msg);
}

__cold C4SliceResult c4error_getDescription(C4Error error) noexcept { return toSliceResult(error.description()); }

__cold char* c4error_getDescriptionC(C4Error error, char buffer[], size_t bufferSize) noexcept {
    string msg = error.description();
    auto   len = min(msg.size(), bufferSize - 1);
    memcpy(buffer, msg.data(), len);
    buffer[len] = '\0';
    return buffer;
}

__cold bool c4error_getCaptureBacktraces() noexcept { return error::sCaptureBacktraces; }

__cold void c4error_setCaptureBacktraces(bool c) noexcept { error::sCaptureBacktraces = c; }

__cold C4StringResult c4error_getBacktrace(C4Error error) noexcept {
    if ( string bt = error.backtrace(); bt.empty() ) return {};
    else
        return toSliceResult(bt);
}

#pragma mark - ERROR UTILITIES:


using CodeList = const int[];
using ErrorSet = const int* [kC4MaxErrorDomainPlus1];

__cold static bool errorIsInSet(const C4Error& err, ErrorSet set) {
    if ( err.code != 0 && (unsigned)err.domain < kC4MaxErrorDomainPlus1 ) {
        const int* pCode = set[err.domain];
        if ( pCode ) {
            for ( ; *pCode != 0; ++pCode )
                if ( *pCode == err.code ) return true;
        }
    }
    return false;
}

__cold bool C4Error::mayBeTransient() const noexcept {
    static CodeList kTransientPOSIX = {ENETRESET, ECONNABORTED, ECONNRESET, ETIMEDOUT, ECONNREFUSED, 0};

    static CodeList kTransientNetwork   = {kC4NetErrDNSFailure,
                                           kC4NetErrTimeout,
                                           kC4NetErrNetworkReset,
                                           kC4NetErrConnectionAborted,
                                           kC4NetErrConnectionReset,
                                           kC4NetErrConnectionRefused,
                                           0};
    static CodeList kTransientWebSocket = {408, /* Request Timeout */
                                           429, /* Too Many Requests (RFC 6585) */
                                           502, /* Bad Gateway */
                                           503, /* Service Unavailable */
                                           504, /* Gateway Timeout */
                                           websocket::kCodeAbnormal,
                                           websocket::kCloseAppTransient,
                                           0};
    static ErrorSet kTransient          = {// indexed by C4ErrorDomain
                                  nullptr, nullptr,           kTransientPOSIX,    nullptr,
                                  nullptr, kTransientNetwork, kTransientWebSocket};
    return errorIsInSet(*this, kTransient);
}

__cold bool C4Error::mayBeNetworkDependent() const noexcept {
    static CodeList kUnreachablePOSIX = {ENETDOWN,     ENETUNREACH,   ENOTCONN, ETIMEDOUT,
#ifndef _MSC_VER
                                         EHOSTDOWN,  // Doesn't exist on Windows
#endif
                                         EHOSTUNREACH, EADDRNOTAVAIL, EPIPE,    0};

    static CodeList kUnreachableNetwork = {
            kC4NetErrDNSFailure,
            kC4NetErrUnknownHost,  // Result may change if user logs into VPN or moves to intranet
            kC4NetErrNetworkDown,
            kC4NetErrNetworkUnreachable,
            kC4NetErrNotConnected,
            kC4NetErrTimeout,
            kC4NetErrHostDown,
            kC4NetErrHostUnreachable,
            kC4NetErrAddressNotAvailable,
            kC4NetErrBrokenPipe,
            kC4NetErrUnknownInterface,
            0};
    static ErrorSet kUnreachable = {// indexed by C4ErrorDomain
                                    nullptr, nullptr, kUnreachablePOSIX, nullptr, nullptr, kUnreachableNetwork,
                                    nullptr};
    return errorIsInSet(*this, kUnreachable);
}

__cold bool c4error_mayBeTransient(C4Error e) noexcept { return e.mayBeTransient(); }

__cold bool c4error_mayBeNetworkDependent(C4Error e) noexcept { return e.mayBeNetworkDependent(); }
