//
//  c4Base.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/1/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "c4Private.h"

#include "Logging.hh"

#include "WebSocketInterface.hh"
#include "SQLiteCpp/Exception.h"
#include <ctype.h>
#include <algorithm>
#include <deque>
#include <mutex>

#ifdef _MSC_VER
#include <winerror.h>
#define EHOSTDOWN WSAEHOSTDOWN
#endif

using namespace litecore;

extern "C" {
    CBL_CORE_API std::atomic_int gC4InstanceCount;
    CBL_CORE_API std::atomic_int gC4ExpectExceptions;
    bool C4ExpectingExceptions();
    bool C4ExpectingExceptions() { return gC4ExpectExceptions > 0; }

}


#pragma mark - ERRORS:


namespace c4Internal {

    static_assert((int)kC4MaxErrorDomainPlus1 == (int)error::NumDomainsPlus1,
                  "C4 error domains are not in sync with C++ ones");
    static_assert((int)kC4NumErrorCodesPlus1 == (int)error::NumLiteCoreErrorsPlus1,
                  "C4 error codes are not in sync with C++ ones");

    // A buffer that stores recently generated error messages, referenced by C4Error.internal_info
    static uint32_t sFirstErrorMessageInternalInfo = 1000;  // internal_info of 1st item in deque
    static deque<string> sErrorMessages;                    // last 10 error message strings
    static mutex sErrorMessagesMutex;


    void recordError(C4ErrorDomain domain, int code, string message, C4Error* outError) noexcept {
        if (outError) {
            outError->domain = domain;
            outError->code = code;
            outError->internal_info = 0;
            if (!message.empty()) {
                try {
                    lock_guard<mutex> lock(sErrorMessagesMutex);
                    sErrorMessages.emplace_back(message);
                    if (sErrorMessages.size() > kMaxErrorMessagesToSave) {
                        sErrorMessages.pop_front();
                        ++sFirstErrorMessageInternalInfo;
                    }
                    outError->internal_info = (uint32_t)(sFirstErrorMessageInternalInfo +
                                                         sErrorMessages.size() - 1);
                } catch (...) { }
            }
        }
    }

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) noexcept {
        recordError(domain, code, string(), outError);
    }

    static string lookupErrorMessage(C4Error &error) {
        lock_guard<mutex> lock(sErrorMessagesMutex);
        int32_t index = error.internal_info - sFirstErrorMessageInternalInfo;
        if (index >= 0 && index < sErrorMessages.size()) {
            return sErrorMessages[index];
        } else {
            return string();
        }
    }
}


C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) C4API {
    C4Error error;
    recordError(domain, code, (string)message, &error);
    return error;
}


void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) C4API {
    recordError(domain, code, (string)message, outError);
}


C4SliceResult c4error_getMessage(C4Error err) noexcept {
    if (err.code == 0) {
        return sliceResult(nullptr);
    } else if (err.domain < 1 || err.domain >= (C4ErrorDomain)error::NumDomainsPlus1) {
        return sliceResult("unknown error domain");
    } else {
        // Custom message referenced in info field?
        string message = lookupErrorMessage(err);
        if (!message.empty())
            return sliceResult(message);
        // No; get the regular error message for this domain/code:
        error e((error::Domain)err.domain, err.code);
        return sliceResult(e.what());
    }
}

char* c4error_getMessageC(C4Error error, char buffer[], size_t bufferSize) noexcept {
    C4SliceResult msg = c4error_getMessage(error);
    auto len = min(msg.size, bufferSize-1);
    if (msg.buf)
        memcpy(buffer, msg.buf, len);
    buffer[len] = '\0';
    c4slice_free(msg);
    return buffer;
}


int c4_getObjectCount() noexcept {
    return gC4InstanceCount;
}


#pragma mark - ERROR UTILITIES:


using CodeList = const int[];
using ErrorSet = const int* [kC4MaxErrorDomainPlus1];


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


bool c4error_mayBeTransient(C4Error err) C4API {
    static CodeList kTransientPOSIX = {
        ENETRESET, ECONNABORTED, ECONNRESET, ETIMEDOUT, ECONNREFUSED, 0};
    static CodeList kTransientNetwork = {
        kC4NetErrDNSFailure,
        0};
    static CodeList kTransientWebSocket = {
        408, /* Request Timeout */
        429, /* Too Many Requests (RFC 6585) */
        500, /* Internal Server Error */
        502, /* Bad Gateway */
        503, /* Service Unavailable */
        504, /* Gateway Timeout */
        websocket::kCodeGoingAway,
        0};
    static ErrorSet kTransient = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kTransientPOSIX,
        nullptr,
        nullptr,
        nullptr,
        kTransientNetwork,
        kTransientWebSocket};
    return errorIsInSet(err, kTransient);
}

bool c4error_mayBeNetworkDependent(C4Error err) C4API {
    static CodeList kUnreachablePOSIX = {
        ENETDOWN, ENETUNREACH, ETIMEDOUT, EHOSTDOWN, EHOSTUNREACH, 0};
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
        nullptr,
        kUnreachableNetwork,
        nullptr};
    return errorIsInSet(err, kUnreachable);
}


#pragma mark - SLICES:


bool c4SliceEqual(C4Slice a, C4Slice b) noexcept {
    return a == b;
}


void c4slice_free(C4SliceResult slice) noexcept {
    alloc_slice::release({slice.buf, slice.size});
}


namespace c4Internal {

    C4SliceResult sliceResult(alloc_slice s) {
        s.retain();
        return {s.buf, s.size};
    }

    C4SliceResult sliceResult(slice s) {
        return sliceResult(alloc_slice(s));
    }

    C4SliceResult sliceResult(const char *str) {
        if (str)
            return sliceResult(slice{str, strlen(str)});
        else
            return {nullptr, 0};
    }

}


#pragma mark - LOGGING:


void c4log_writeToCallback(C4LogLevel level, C4LogCallback callback, bool preformatted) noexcept {
    LogDomain::setCallback((LogDomain::Callback_t)callback, preformatted, (LogLevel)level);
}


bool c4log_writeToBinaryFile(C4LogLevel level, C4String path, C4Error *outError) noexcept {
    return tryCatch(outError, [=] {
        LogDomain::writeEncodedLogsTo(slice(path).asString(), (LogLevel)level);
    });
}

C4LogLevel c4log_callbackLevel() noexcept        {return (C4LogLevel)LogDomain::callbackLogLevel();}
C4LogLevel c4log_binaryFileLevel() noexcept      {return (C4LogLevel)LogDomain::fileLogLevel();}

void c4log_setCallbackLevel(C4LogLevel level) noexcept   {LogDomain::setCallbackLogLevel((LogLevel)level);}
void c4log_setBinaryFileLevel(C4LogLevel level) noexcept {LogDomain::setFileLogLevel((LogLevel)level);}


CBL_CORE_API const C4LogDomain kC4DefaultLog = (C4LogDomain)&DefaultLog;


C4LogDomain c4log_getDomain(const char *name, bool create) noexcept {
    if (!name)
        return kC4DefaultLog;
    auto domain = LogDomain::named(name);
    if (!domain && create)
        domain = new LogDomain(name);
    return (C4LogDomain)domain;
}


const char* c4log_getDomainName(C4LogDomain c4Domain) noexcept {
    auto domain = (LogDomain*)c4Domain;
    return domain->name();
}


C4LogLevel c4log_getLevel(C4LogDomain c4Domain) noexcept {
    auto domain = (LogDomain*)c4Domain;
    return (C4LogLevel) domain->level();
}


void c4log_setLevel(C4LogDomain c4Domain, C4LogLevel level) noexcept {
    auto domain = (LogDomain*)c4Domain;
    domain->setLevel((LogLevel)level);
}


void c4log_warnOnErrors(bool warn) noexcept {
    error::sWarnOnError = warn;
}


void c4log(C4LogDomain c4Domain, C4LogLevel level, const char *fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    c4vlog(c4Domain, level, fmt, args);
    va_end(args);
}


void c4vlog(C4LogDomain c4Domain, C4LogLevel level, const char *fmt, va_list args) noexcept {
    try {
        ((LogDomain*)c4Domain)->vlog((LogLevel)level, fmt, args);
    } catch (...) { }
}
