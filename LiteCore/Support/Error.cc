//
// Error.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "Error.hh"
#include "Logging.hh"
#include "FleeceException.hh"
#include "PlatformIO.hh"
#include "StringUtil.hh"
#include <sqlite3.h>
#include <SQLiteCpp/Exception.h>
#include "WebSocketInterface.hh"    // for Network error codes
#include <cerrno>
#include <string>
#include <sstream>

#ifdef _MSC_VER
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#if __ANDROID__
#include <android/log.h>
#endif

#if defined(__clang__) && !defined(__ANDROID__) // For logBacktrace:
#include <execinfo.h>   // Not available in Windows?
#include <cxxabi.h>
#endif

#ifdef LITECORE_IMPL
#include <mbedtls/error.h>
#include <sockpp/exception.h>
#else
#include "c4Base.h"     // Ugly layering violation, but needed for using Error in other libs
#include "c4Private.h"
#endif

namespace litecore {

    using namespace std;

#ifdef _WIN32
    static string cbl_strerror(int err) {
        if(err < sys_nerr) {
            // As of Windows 10, only errors 0 - 42 have a message in strerror
            return strerror(err);
        }

        // Hope the POSIX definitions don't change...
        if(err < 100 || err > 140) {
            return "Unknown Error";
        }

        static long wsaEquivalent[] = {
            WSAEADDRINUSE,
            WSAEADDRNOTAVAIL,
            WSAEAFNOSUPPORT,
            WSAEALREADY,
            0,
            WSAECANCELLED,
            WSAECONNABORTED,
            WSAECONNREFUSED,
            WSAECONNRESET,
            WSAEDESTADDRREQ,
            WSAEHOSTUNREACH,
            0,
            WSAEINPROGRESS,
            WSAEISCONN,
            WSAELOOP,
            WSAEMSGSIZE,
            WSAENETDOWN,
            WSAENETRESET,
            WSAENETUNREACH,
            WSAENOBUFS,
            0,
            0,
            0,
            WSAENOPROTOOPT,
            0,
            0,
            WSAENOTCONN,
            0,
            WSAENOTSOCK,
            0,
            WSAEOPNOTSUPP,
            0,
            0,
            0,
            0,
            WSAEPROTONOSUPPORT,
            WSAEPROTOTYPE,
            0,
            WSAETIMEDOUT,
            0,
            WSAEWOULDBLOCK
        };

        const long equivalent = wsaEquivalent[err - 100];
        if(equivalent == 0) {
            return "Unknown Error";
        }

        char buf[1024];
        buf[0] = '\x0';
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, equivalent, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			buf, sizeof(buf), nullptr);
        return string(buf);
    }
#else
#define cbl_strerror strerror
#endif


#pragma mark ERROR CODES, NAMES, etc.


    struct codeMapping { int err; error::Domain domain; int code; };

    static const codeMapping kPOSIXMapping[] = {
        {ENOENT,                        error::LiteCore,    error::NotFound},
        {0, /*must end with err=0*/     error::LiteCore,    0},
    };

    static const codeMapping kSQLiteMapping[] = {
        {SQLITE_PERM,                   error::LiteCore,    error::NotWriteable},
        {SQLITE_BUSY,                   error::LiteCore,    error::Busy},
        {SQLITE_LOCKED,                 error::LiteCore,    error::Busy},
        {SQLITE_NOMEM,                  error::LiteCore,    error::MemoryError},
        {SQLITE_READONLY,               error::LiteCore,    error::NotWriteable},
        {SQLITE_IOERR,                  error::LiteCore,    error::IOError},
        {SQLITE_CORRUPT,                error::LiteCore,    error::CorruptData},
        {SQLITE_FULL,                   error::POSIX,       ENOSPC},
        {SQLITE_CANTOPEN,               error::LiteCore,    error::CantOpenFile},
        {SQLITE_NOTADB,                 error::LiteCore,    error::NotADatabaseFile},
        {SQLITE_PERM,                   error::LiteCore,    error::NotWriteable},
        {0, /*must end with err=0*/     error::LiteCore,    0},
    };
    //TODO: Map the SQLite 'extended error codes' that give more detail about file errors

    static const codeMapping kFleeceMapping[] = {
        {fleece::MemoryError,           error::LiteCore,    error::MemoryError},
        {fleece::JSONError,             error::LiteCore,    error::InvalidQuery},
        {fleece::PathSyntaxError,       error::LiteCore,    error::InvalidQuery},
        {0, /*must end with err=0*/     error::Fleece,      0},
    };

    static bool mapError(error::Domain &domain, int &code, const codeMapping table[]) {
        for (const codeMapping *row = &table[0]; row->err != 0; ++row) {
            if (row->err == code) {
                domain = row->domain;
                code = row->code;
                return true;
            }
        }
        return false;
    }

    static int getPrimaryCode(const error::Domain &domain, const int& code)
    {
        if(domain != error::Domain::SQLite) {
            return code;
        }

        return code & 0xff;
    }

#ifdef LITECORE_IMPL
    static const char* litecore_errstr(error::LiteCoreError code) {
        static const char* kLiteCoreMessages[] = {
            // These must match up with the codes in the declaration of LiteCoreError
            "no error", // 0
            "assertion failed",
            "unimplemented function called",
            "unsupported encryption algorithm",
            "bad revision ID",
            "corrupt revision data",
            "database not open",
            "not found",
            "conflict",
            "invalid parameter",
            "unexpected exception",
            "can't open file",
            "file I/O error",
            "memory allocation failed",
            "not writeable",
            "data is corrupted",
            "database busy/locked",
            "must be called during a transaction",
            "transaction not closed",
            "unsupported operation for this database type",
            "file is not a database (or encryption key is invalid/missing)",
            "file/data is not in the requested format",
            "encryption/decryption error",
            "query syntax error",
            "missing database index",
            "invalid query parameter name/number",
            "error on remote server",
            "database is in an old file format that can't be opened",
            "database is in a newer file format than this software supports",
            "invalid document ID",
            "database cannot be upgraded to the current version", // 30
            "can't apply document delta: base revision body unavailable",
            "can't apply document delta: format is invalid",
        };
        static_assert(sizeof(kLiteCoreMessages)/sizeof(kLiteCoreMessages[0]) ==
                        error::NumLiteCoreErrorsPlus1, "Incomplete error message table");
        const char *str = nullptr;
        if (code < sizeof(kLiteCoreMessages)/sizeof(char*))
            str = kLiteCoreMessages[code];
        if (!str)
            str = "(unknown LiteCoreError)";
        return str;
    }

    static const char* fleece_errstr(fleece::ErrorCode code) {
        static const char* kFleeceMessages[] = {
            // These must match up with the codes in the declaration of FLError
            "no error", // 0
            "memory error",
            "out of range",
            "invalid data",
            "Fleece encode/decode error",
            "JSON encode/decode error",
            "unparseable Fleece value",
            "path syntax error",
            "internal error",
            "item not found",
            "misuse of Fleece shared-keys API",
        };
        const char *str = nullptr;
        if (code < sizeof(kFleeceMessages)/sizeof(char*))
            str = kFleeceMessages[code];
        if (!str)
            str = "(unknown Fleece error)";
        return str;
    }

    static const char* network_errstr(int code) {
        static const char* kNetworkMessages[] = {
            // These must match up with the codes in the NetworkError enum in WebSocketInterface.hh
            "no error", // 0
            "DNS error",
            "unknown hostname",
            "connection timed out",
            "invalid URL",
            "too many redirects",
            "TLS connection failed",
            "server TLS certificate expired",
            "server TLS certificate untrusted",
            "server requires a TLS client certificate",
            "server rejected the TLS client certificate",
            "server TLS certificate is self-signed or has unknown root cert",
            "invalid HTTP redirect, or redirect loop",
            "unknown network error",
            "server TLS certificate has been revoked",
            "server TLS certificate name mismatch"
        };
        const char *str = nullptr;
        if (code < sizeof(kNetworkMessages)/sizeof(char*))
            str = kNetworkMessages[code];
        if (!str)
            str = "(unknown network error)";
        return str;
    }

    static const char* websocket_errstr(int code) {
        static const struct {int code; const char* message;} kWebSocketMessages[] = {
            {400, "invalid request"},
            {404, "not found"},
            {404, "not found"},
            {409, "conflict"},
            {410, "gone"},
            {500, "server error"},
            {502, "remote error"},
            {1000, "normal close"},
            {1001, "peer going away"},
            {1002, "protocol error"},
            {1003, "unsupported data"},
            {1004, "reserved"},
            {1005, "no status code received"},
            {1006, "connection closed abnormally"},
            {1007, "inconsistent data"},
            {1008, "policy violation"},
            {1009, "message too big"},
            {1010, "extension not negotiated"},
            {1011, "unexpected condition"},
            {1015, "TLS handshake failed"},
            {0, nullptr}
        };

        for (unsigned i = 0; kWebSocketMessages[i].message; ++i) {
            if (kWebSocketMessages[i].code == code)
                return kWebSocketMessages[i].message;
        }
        return code >= 1000 ? "(unknown WebSocket status)" : "(unknown HTTP status)";
    }
#endif // LITECORE_IMPL

    string error::_what(error::Domain domain, int code) noexcept {
#ifdef LITECORE_IMPL
        switch (domain) {
            case LiteCore:
                return litecore_errstr((LiteCoreError)code);
            case POSIX:
                return cbl_strerror(code);
            case SQLite:
            {
                const int primary = code & 0xFF;
                if(code == primary) {
                    return sqlite3_errstr(code);
                }

                stringstream ss;
                ss << sqlite3_errstr(primary) << " (" << code << ")";
                return ss.str();
            }
            case Fleece:
                return fleece_errstr((fleece::ErrorCode)code);
            case Network:
                return network_errstr(code);
            case WebSocket:
                return websocket_errstr(code);
            case MbedTLS: {
#ifdef LITECORE_IMPL
                char buf[100];
                mbedtls_strerror(code, buf, sizeof(buf));
                return string(buf);
#else
                return format("(mbedTLS %s0x%x)", (code < 0 ? "-" : ""), abs(code));
#endif
            }
            default:
                return "unknown error domain";
        }
#else
        C4StringResult msg = c4error_getMessage({(C4ErrorDomain)domain, code});
        string result((const char*)msg.buf, msg.size);
        c4slice_free(msg);
        return result;
#endif
    }


    const char* error::nameOfDomain(Domain domain) noexcept {
        // Indexed by Domain
        static const char* kDomainNames[] = {"0",
                                             "LiteCore", "POSIX", "SQLite", "Fleece",
                                             "Network", "WebSocket", "mbedTLS"};
        static_assert(sizeof(kDomainNames)/sizeof(kDomainNames[0]) == error::NumDomainsPlus1,
                      "Incomplete domain name table");
        
        if (domain < 0 || domain >= NumDomainsPlus1)
            return "INVALID_DOMAIN";
        return kDomainNames[domain];
    }


#pragma mark - ERROR CLASS:


    bool error::sWarnOnError = false;

    
    error::error(error::Domain d, int c)
    :error(d, c, _what(d, c))
    { }


    error::error(error::Domain d, int c, const std::string &what)
    :runtime_error(what),
    domain(d),
    code(getPrimaryCode(d, c))
    { }


    error& error::operator= (const error &e) {
        // This has to be hacked, since `domain` and `code` are marked `const`.
        this->~error();
        new (this) error(e);
        return *this;
    }


    error error::standardized() const {
        Domain d = domain;
        int c = code;
        switch (domain) {
            case POSIX:
                mapError(d, c, kPOSIXMapping);
                break;
            case SQLite:
                mapError(d, c, kSQLiteMapping);
                break;
            case Fleece:
                mapError(d, c, kFleeceMapping);
                break;
            default:
                return *this;
        }
        return error(d, c);
    }


    static error unexpectedException(const std::exception &x) {
        // Get the actual exception class name using RTTI.
        // Unmangle it by skipping class name prefix like "St12" (may be compiler dependent)
        const char *name =  typeid(x).name();
        while (isalpha(*name)) ++name;
        while (isdigit(*name)) ++name;
        Warn("Caught unexpected C++ %s(\"%s\")", name, x.what());
        return error(error::LiteCore, error::UnexpectedError, x.what());
    }


    error error::convertRuntimeError(const std::runtime_error &re) {
        if (auto e = dynamic_cast<const error*>(&re); e) {
            return *e;
        } else if (auto se = dynamic_cast<const SQLite::Exception*>(&re); se) {
            return error(SQLite, se->getExtendedErrorCode(), se->what());
        } else if (auto fe = dynamic_cast<const fleece::FleeceException*>(&re); fe) {
            return error(Fleece, fe->code, fe->what());
#ifdef LITECORE_IMPL
        } else if (auto syserr = dynamic_cast<const sockpp::sys_error*>(&re); syserr) {
            int code = syserr->error();
            return error((code < 0 ? MbedTLS : POSIX), code);
        } else if (auto gx = dynamic_cast<const sockpp::getaddrinfo_error*>(&re); gx) {
            if (gx->error() == EAI_NONAME || gx->error() == HOST_NOT_FOUND) {
                return error(Network, websocket::kNetErrUnknownHost,
                             "Unknown hostname \"" + gx->hostname() + "\"");
            } else {
                return error(Network, websocket::kNetErrDNSFailure,
                             "Error resolving hostname \"" + gx->hostname() + "\": " + gx->what());
            }
#endif
        } else {
            return unexpectedException(re);
        }
    }

    error error::convertException(const std::exception &x) {
        if (auto re = dynamic_cast<const std::runtime_error*>(&x); re)
            return convertRuntimeError(*re);
        else
            return unexpectedException(x);
    }


    bool error::isUnremarkable() const {
        if (code == 0)
            return true;
        switch (domain) {
            case LiteCore:
                return code == NotFound || code == DatabaseTooOld;
            case POSIX:
                return code == ENOENT;
            case Network:
                return code != websocket::kNetErrUnknown;
            default:
                return false;
        }
    }


    void error::_throw() {
#ifdef LITECORE_IMPL
        bool warn = sWarnOnError;
#else
        bool warn = c4log_getWarnOnErrors();
#endif
        if (warn && !isUnremarkable()) {
            WarnError("LiteCore throwing %s error %d: %s%s",
                      nameOfDomain(domain), code, what(), backtrace(1).c_str());
        }
        throw *this;
    }

    
    void error::_throw(Domain domain, int code ) {
        DebugAssert(code != 0);
        error{domain, code}._throw();
    }

    
    void error::_throw(error::LiteCoreError err) {
        _throw(LiteCore, err);
    }

    void error::_throwErrno() {
        _throw(POSIX, errno);
    }


    void error::_throw(error::LiteCoreError code, const char *fmt, ...) {
        char *msg = nullptr;
        va_list args;
        va_start(args, fmt);
        int len = vasprintf(&msg, fmt, args);
        va_end(args);
        std::string message;
        if (len >= 0) {
            message = msg;
            free(msg);
        }
        error{LiteCore, code, message}._throw();
    }


    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr,
                                const char *message)
    {
        if (!message)
            message = expr;
        if (!WillLog(LogLevel::Error))
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", message, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)%s",
                  message, file, line, fn, backtrace(1).c_str());
        throw error(error::AssertionFailed);
    }

#if !defined(__ANDROID__) && !defined(_MSC_VER)
    /*static*/ string error::backtrace(unsigned skip) {
#ifdef __clang__
        ++skip;     // skip the logBacktrace frame itself
        void* addrs[50];
        int n = ::backtrace(addrs, 50) - skip;
        if (n <= 0)
            return "";
        char** lines = backtrace_symbols(&addrs[skip], n);
        char* unmangled = nullptr;
        size_t unmangledLen = 0;

        stringstream out;

        for (int i = 0; i < n; ++i) {
            out << "\n\t";
            char library[101], functionBuf[201];
            size_t pc;
            int offset;
            if (sscanf(lines[i], "%*d %100s %zi %200s + %i",
                       library, &pc, functionBuf, &offset) == 4 ||
                sscanf(lines[i], "%100[^(](%200[^+]+%i) ""[""%zi""]",
                       library, functionBuf, &offset, &pc) == 4) {
                const char *function = functionBuf;
                int status;
                unmangled = abi::__cxa_demangle(function, unmangled, &unmangledLen, &status);
                if (unmangled && status == 0)
                    function = unmangled;
                char *cstr = nullptr;
                if (asprintf(&cstr, "%2d  %-25s %s + %d", i, library, function, offset) < 0)
                    return "(error printing backtrace)";
                out << cstr;
                free(cstr);
            } else {
                out << lines[i];
            }
        }
        free(unmangled);
        free(lines);
        return out.str();
#else
        return " (no backtrace available)";
#endif
    }
#endif
}

