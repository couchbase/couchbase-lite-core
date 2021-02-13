//
// c4Error.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "c4Internal.hh"
#include "c4Private.h"
#include "Backtrace.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "WebSocketInterface.hh"    // For WebSocket error codes
#include <deque>
#include <exception>
#include <mutex>
#include <sstream>

using namespace std;
using namespace litecore;

using Backtrace = fleece::Backtrace;


namespace c4Internal {

    static_assert((int)kC4MaxErrorDomainPlus1 == (int)error::NumDomainsPlus1,
                  "C4 error domains are not in sync with C++ ones");
    static_assert((int)kC4NumErrorCodesPlus1 == (int)error::NumLiteCoreErrorsPlus1,
                  "C4 error codes are not in sync with C++ ones");
    static_assert((int)kC4NumNetErrorCodesPlus1 == (int)litecore::websocket::kNetErrorMaxPlus1,
                  "C4 net error codes are not in sync with C++ ones");


#pragma mark - ERROR INFO:


    // A buffer that stores recently generated error messages, referenced by C4Error.internal_info
    struct c4ErrorInfo {
        string message;
        shared_ptr<Backtrace> backtrace;
    };

    static uint32_t sFirstErrorMessageInternalInfo = 1000;  // internal_info of 1st item in deque
    static deque<c4ErrorInfo> sErrorMessages;               // latest 10 error message infos
    static mutex sErrorMessagesMutex;                       // mutex guarding the above


    // Must be called while holding the sErrorMessagesMutex
    __cold
    static c4ErrorInfo* _getErrorInfo(C4Error &error, bool create) {
        int32_t index = error.internal_info - sFirstErrorMessageInternalInfo;
        if (index >= 0 && index < sErrorMessages.size()) {
            return &sErrorMessages[index];
        } else if (create) {
            if (sErrorMessages.size() >= kMaxErrorMessagesToSave) {
                sErrorMessages.pop_front();
                ++sFirstErrorMessageInternalInfo;
            }
            sErrorMessages.push_back(c4ErrorInfo());
            error.internal_info = (uint32_t)(sFirstErrorMessageInternalInfo +
                                             sErrorMessages.size() - 1);
            return &sErrorMessages.back();
        } else {
            return nullptr;
        }
    }


    __cold
    static optional<c4ErrorInfo> copyErrorInfo(C4Error &error) {
        lock_guard<mutex> lock(sErrorMessagesMutex);
        if (auto infop = _getErrorInfo(error, false))
            return *infop;
        return nullopt;
    }


#pragma mark - INTERNAL API FOR CREATING C4ERRORS:


    // Core function that creates/initializes a C4Error.
    __cold
    static C4Error makeError(C4ErrorDomain domain, int code,
                             c4ErrorInfo info,
                             unsigned skip =0) noexcept
    {
        C4Error error {domain, code, 0};
        if (error::sCaptureBacktraces && !info.backtrace)
            info.backtrace = Backtrace::capture(skip + 2); // don't capture this frame or its caller
        if (!info.message.empty() || info.backtrace) {
            try {
                lock_guard<mutex> lock(sErrorMessagesMutex);
                *_getErrorInfo(error, true) = move(info);
            } catch (...) { }
        }
        return error;
    }


    __cold
    static C4Error vmakeError(C4ErrorDomain domain, int code,
                              const char *format, va_list args,
                              unsigned skip =0) noexcept
    {
        string message;
        if (format && *format) {
            try {
                message = vformat(format, args);
            } catch (...) { }
        }
        return makeError(domain, code, {move(message), nullptr}, skip + 1);
    }


    __cold
    void recordError(C4ErrorDomain domain, int code, string_view message, C4Error* outError) noexcept {
        if (outError)
            *outError = makeError(domain, code, {string(message)});
    }


    __cold
    void recordError(C4ErrorDomain domain, int code, C4Error* outError) noexcept {
        if (outError)
            *outError = makeError(domain, code, {});
    }


    __cold
    void recordError(C4Error *outError, C4ErrorDomain domain, int code,
                     const char *format, ...) noexcept
    {
        if (outError) {
            va_list args;
            va_start(args, format);
            *outError = vmakeError(domain, code, format, args);
            va_end(args);
        }
    }


    __cold
    void recordException(const exception &x, C4Error* outError) noexcept {
        if (outError) {
            error e = error::convertException(x).standardized();
            *outError = makeError((C4ErrorDomain)e.domain, e.code, {e.what(), e.backtrace});
        }
    }


    __cold
    void recordException(C4Error* outError) noexcept {
        if (outError) {
            // This rigamarole recovers the current exception being thrown...
            auto xp = std::current_exception();
            if (xp) {
                try {
                    std::rethrow_exception(xp);
                } catch(const std::exception& x) {
                    // Now we have the exception, so we can record it in outError:
                    recordException(x, outError);
                    return;
                } catch (...) { }
            }
            shared_ptr<Backtrace> bt(new Backtrace(1));
            *outError = makeError(LiteCoreDomain, kC4ErrorUnexpectedError,
                                  {"Unknown C++ exception", bt});
        }
    }

}

using namespace c4Internal;


#pragma mark - PUBLIC API FOR CREATING C4ERRORS:


__cold
C4Error c4error_vprintf(C4ErrorDomain domain, int code, const char *format, va_list args) noexcept {
    return vmakeError(domain, code, format, args);
}


__cold
C4Error c4error_printf(C4ErrorDomain domain, int code, const char *format, ...) noexcept {
    va_list args;
    va_start(args, format);
    C4Error error = vmakeError(domain, code, format, args);
    va_end(args);
    return error;
}


__cold
C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) noexcept {
    string messageStr;
    if (message.size > 0)
        messageStr = string(slice(message));
    return makeError(domain, code, {move(messageStr), nullptr});
}


__cold
void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) noexcept {
    if (outError)
        *outError = makeError(domain, code, {string(slice(message))});
}


#pragma mark - PUBLIC ERROR ACCESSORS:


__cold
static string getErrorMessage(C4Error err) {
    if (err.code == 0) {
        return "";
    } else if (err.domain < 1 || err.domain >= (C4ErrorDomain)error::NumDomainsPlus1) {
        return "invalid C4Error (unknown domain)";
    } else if (auto info = copyErrorInfo(err); info && !info->message.empty()) {
        // Custom message referenced in info field
        return info->message;
    } else {
        // No; get the regular error message for this domain/code:
        return error((error::Domain)err.domain, err.code).what();
    }
}


__cold
static const char* getErrorName(C4Error err) {
    static constexpr const char* kLiteCoreNames[] = {
        // These must match up with the codes in the declaration of LiteCoreError
        "no error", // 0
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
    static_assert(sizeof(kLiteCoreNames)/sizeof(kLiteCoreNames[0]) ==
                  error::NumLiteCoreErrorsPlus1, "Incomplete error message table");

    if (err.domain == LiteCoreDomain && err.code < sizeof(kLiteCoreNames)/sizeof(char*))
        return kLiteCoreNames[err.code];
    else
        return nullptr;
}


__cold
C4SliceResult c4error_getMessage(C4Error err) noexcept {
    if (string msg = getErrorMessage(err); msg.empty())
        return {};
    else
        return sliceResult(msg);
}


__cold
C4SliceResult c4error_getDescription(C4Error error) noexcept {
    if (error.code == 0)
        return sliceResult("No error");
    const char *errName = getErrorName(error);
    stringstream str;
    str << error::nameOfDomain((error::Domain)error.domain) << " ";
    if (errName)
        str << errName;
    else
        str << "error " << error.code;
    str << ", \"" << getErrorMessage(error) << "\"";
    return sliceResult(str.str());
}


__cold
char* c4error_getDescriptionC(C4Error error, char buffer[], size_t bufferSize) noexcept {
    C4SliceResult msg = c4error_getDescription(error);
    auto len = min(msg.size, bufferSize-1);
    if (msg.buf)
        memcpy(buffer, msg.buf, len);
    buffer[len] = '\0';
    c4slice_free(msg);
    return buffer;
}


bool c4error_getCaptureBacktraces() noexcept             {return error::sCaptureBacktraces;}
void c4error_setCaptureBacktraces(bool capture) noexcept {error::sCaptureBacktraces = capture;}


C4StringResult c4error_getBacktrace(C4Error error) noexcept {
    if (auto info = copyErrorInfo(error); info && info->backtrace)
        return sliceResult(info->backtrace->toString());
    return {};
}


#pragma mark - ERROR UTILITIES:


using CodeList = const int[];
using ErrorSet = const int* [kC4MaxErrorDomainPlus1];


__cold
static bool errorIsInSet(C4Error err, ErrorSet set) {
    if (err.code != 0 && (unsigned)err.domain < kC4MaxErrorDomainPlus1) {
        const int *pCode = set[err.domain];
        if (pCode) {
            for (; *pCode != 0; ++pCode)
                if (*pCode == err.code)
                    return true;
        }
    }
    return false;
}


__cold
bool c4error_mayBeTransient(C4Error err) noexcept {
    static CodeList kTransientPOSIX = {
        ENETRESET, ECONNABORTED, ECONNRESET, ETIMEDOUT, ECONNREFUSED, 0};

    static CodeList kTransientNetwork = {
        kC4NetErrDNSFailure,
        kC4NetErrTimeout,
        0};
    static CodeList kTransientWebSocket = {
        408, /* Request Timeout */
        429, /* Too Many Requests (RFC 6585) */
        502, /* Bad Gateway */
        503, /* Service Unavailable */
        504, /* Gateway Timeout */
        websocket::kCodeAbnormal,
        websocket::kCloseAppTransient,
        0};
    static ErrorSet kTransient = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kTransientPOSIX,
        nullptr,
        nullptr,
        kTransientNetwork,
        kTransientWebSocket};
    return errorIsInSet(err, kTransient);
}


__cold
bool c4error_mayBeNetworkDependent(C4Error err) noexcept {
    static CodeList kUnreachablePOSIX = {
        ENETDOWN, ENETUNREACH, ENOTCONN, ETIMEDOUT,
#ifndef _MSC_VER
        EHOSTDOWN, // Doesn't exist on Windows
#endif
        EHOSTUNREACH,EADDRNOTAVAIL,
        EPIPE, 0};

    static CodeList kUnreachableNetwork = {
        kC4NetErrDNSFailure,
        kC4NetErrUnknownHost,   // Result may change if user logs into VPN or moves to intranet
        0};
    static ErrorSet kUnreachable = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kUnreachablePOSIX,
        nullptr,
        nullptr,
        kUnreachableNetwork,
        nullptr};
    return errorIsInSet(err, kUnreachable);
}
