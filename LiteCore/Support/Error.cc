//
// Error.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Error.hh"
#include "Logging.hh"
#include "Backtrace.hh"
#include "FleeceException.hh"
#include "PlatformIO.hh"
#include "StringUtil.hh"
#include "betterassert.hh"
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

#if defined(_WIN32) && defined(LITECORE_IMPL)
    static string win32_message(int err) {
        char buf[1024];
        buf[0] = '\x0';
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			buf, sizeof(buf), nullptr);
        return string(buf);
    }

    static string cbl_strerror(int err) {
        if(err < sys_nerr) {
            // As of Windows 10, only errors 0 - 42 have a message in strerror
            return strerror(err);
        }

        // Hope the POSIX definitions don't change...
        if(err < 100 || err > 140) {
        	// Save 100-140 for the below logic
            return win32_message(err);
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
            // WSA error codes are between 10000 and 11999
            return "Unknown Error";
        }

        return win32_message(equivalent);
    }
#else
#define cbl_strerror strerror
#endif


#pragma mark ERROR CODES, NAMES, etc.


    struct codeMapping { int err; error::Domain domain; int code; };

#ifdef WIN32
#define MAPWSA(code) { WSA##code,error::POSIX,code }
#define MAPWSATO(from, to) { from,error::POSIX,to }

    static const codeMapping kPrimaryCodeMapping[] = {
        MAPWSA(EADDRINUSE),
        MAPWSA(EADDRNOTAVAIL),
        MAPWSA(EAFNOSUPPORT),
        MAPWSA(EALREADY),
        MAPWSATO(WSAECANCELLED, ECANCELED),
        MAPWSA(ECONNABORTED),
        MAPWSA(ECONNREFUSED),
        MAPWSA(ECONNRESET),
        MAPWSA(EDESTADDRREQ),
        MAPWSA(EHOSTUNREACH),
        MAPWSA(EINPROGRESS),
        MAPWSA(EISCONN),
        MAPWSA(ELOOP),
        MAPWSA(EMSGSIZE),
        MAPWSA(ENETDOWN),
        MAPWSA(ENETRESET),
        MAPWSA(ENETUNREACH),
        MAPWSA(ENOBUFS),
        MAPWSA(ENOPROTOOPT),
        MAPWSA(ENOTCONN),
        MAPWSA(ENOTSOCK),
        MAPWSA(EOPNOTSUPP),
        MAPWSA(EPROTONOSUPPORT),
        MAPWSA(EPROTOTYPE),
        MAPWSA(ETIMEDOUT),
        MAPWSA(EWOULDBLOCK),
        {0, /*must end with err=0*/     error::LiteCore,    0}
    };
#endif


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

    __cold
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

    __cold
    static int getPrimaryCode(const error::Domain &domain, const int& code)
    {
#ifdef WIN32
        if(domain == error::Domain::POSIX) {
            for (const codeMapping *row = &kPrimaryCodeMapping[0]; row->err != 0; ++row) {
                if (row->err == code) {
                    return row->code;
                }
            }
        }
#endif

        if(domain != error::Domain::SQLite) {
            return code;
        }

        return code & 0xff;
    }

#ifdef LITECORE_IMPL
    __cold
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

    __cold
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

    __cold
    static const char* network_errstr(int code) {
        static const char* kNetworkMessages[] = {
            // These must match up with the codes in the NetworkError enum in WebSocketInterface.hh
            // The wording is from a client's perspective, i.e. the peer is referred to as "server";
            // these errors do occur on the server side but are not reported by C4Listener.
            "no error", // 0
            "DNS error",
            "unknown hostname",
            "connection timed out",
            "invalid URL",
            "too many redirects",
            "TLS handshake failed",
            "server TLS certificate expired",
            "server TLS certificate untrusted",
            "server requires a TLS client certificate",
            "server rejected the TLS client certificate",
            "server TLS certificate is self-signed or has unknown root cert",
            "redirected to an invalid URL",
            "unknown network error",
            "server TLS certificate has been revoked",
            "server TLS certificate name mismatch",
            "network subsystem was reset",
            "connection aborted",
            "connection reset",
            "connection refused",
            "network subsystem down",
            "network unreachable",
            "socket not connected",
            "host reported not available",
            "host not reachable",
            "address not available",
            "broken pipe"
        };
        const char *str = nullptr;
        if (code < sizeof(kNetworkMessages)/sizeof(char*))
            str = kNetworkMessages[code];
        if (!str)
            str = "(unknown network error)";
        return str;
    }

    __cold
    static const char* websocket_errstr(int code) {
        static const struct {int code; const char* message;} kWebSocketMessages[] = {
            {400, "invalid request"},
            {401, "unauthorized"},
            {403, "forbidden"},
            {404, "not found"},
            {405, "HTTP method not allowed"},
            {409, "conflict"},
            {410, "gone"},
            {500, "server error"},
            {501, "server error: not implemented"},
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
        return code >= 1000 ? "WebSocket error" : "HTTP error";
    }
#endif // LITECORE_IMPL

    __cold
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
                char buf[100];
                mbedtls_strerror(code, buf, sizeof(buf));
                return string(buf);
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


    __cold
    const char* error::nameOfDomain(Domain domain) noexcept {
        // Indexed by Domain
        static const char* kDomainNames[] = {"0",
                                             "LiteCore", "POSIX", "SQLite", "Fleece",
                                             "Network", "WebSocket", "mbedTLS"};
        static_assert(sizeof(kDomainNames)/sizeof(kDomainNames[0]) == error::NumDomainsPlus1,
                      "Incomplete domain name table");
        
        if (domain >= NumDomainsPlus1)
            return "INVALID_DOMAIN";
        return kDomainNames[domain];
    }


#pragma mark - ERROR CLASS:


#ifdef LITECORE_IMPL
    bool error::sWarnOnError = false;
    bool error::sCaptureBacktraces = false;
#else
    #define sWarnOnError c4log_getWarnOnErrors()
    #define sCaptureBacktraces c4error_getCaptureBacktraces()
#endif
    
    __cold
    error::error(error::Domain d, int c)
    :error(d, c, _what(d, c))
    {
        DebugAssert(c != 0);
    }


    __cold
    error::error(error::Domain d, int c, const std::string &what)
    :runtime_error(what),
    domain(d),
    code(getPrimaryCode(d, c))
    {
        DebugAssert(code != 0);
        if (sCaptureBacktraces)
            captureBacktrace(3);
    }


    __cold
    error& error::operator= (const error &e) {
        // This has to be hacked, since `domain` and `code` are marked `const`.
        this->~error();
        new (this) error(e);
        return *this;
    }


    __cold
    void error::captureBacktrace(unsigned skipFrames) {
        if (!backtrace)
            backtrace = fleece::Backtrace::capture(skipFrames + 1);
    }


    __cold
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

        if(domain == d && code == c) {
            // No change, just return the same object
            return *this;
        }

        error err(d, c);
        err.backtrace = backtrace;
        return err;
    }


    __cold
    static error unexpectedException(const std::exception &x) {
        // Get the actual exception class name using RTTI.
        // Unmangle it by skipping class name prefix like "St12" (may be compiler dependent)
        const char *name =  typeid(x).name();
        while (isalpha(*name)) ++name;
        while (isdigit(*name)) ++name;
        Warn("Caught unexpected C++ %s(\"%s\")", name, x.what());
        auto err = error(error::LiteCore, error::UnexpectedError, x.what());
        err.captureBacktrace();     // always get backtrace of unexpected exceptions
        return err;
    }


    __cold
    error error::convertRuntimeError(const std::runtime_error &re) {
        const char *what = re.what();
        if (auto e = dynamic_cast<const error*>(&re); e) {
            return *e;
        } else if (auto iae = dynamic_cast<const invalid_argument*>(&re); iae) {
            return error(LiteCore, InvalidParameter, what);
        } else if (auto faf = dynamic_cast<const fleece::assertion_failure*>(&re); faf) {
            return error(LiteCore, AssertionFailed, what);
        } else if (auto se = dynamic_cast<const SQLite::Exception*>(&re); se) {
            return error(SQLite, se->getExtendedErrorCode(), what);
        } else if (auto fe = dynamic_cast<const fleece::FleeceException*>(&re); fe) {
            error err(Fleece, fe->code, what);
            err.backtrace = fe->backtrace;
            return err;
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
                             "Error resolving hostname \"" + gx->hostname() + "\": " + what);
            }
#endif
        } else {
            return unexpectedException(re);
        }
    }

    __cold
    error error::convertException(const std::exception &x) {
        if (auto re = dynamic_cast<const std::runtime_error*>(&x); re)
            return convertRuntimeError(*re);
        if (auto le = dynamic_cast<const std::logic_error*>(&x); le) {
            LiteCoreError code = AssertionFailed;
            if (dynamic_cast<const std::invalid_argument*>(le) != nullptr
                    || dynamic_cast<const std::domain_error*>(le) != nullptr)
                code = InvalidParameter;
            return error(LiteCore, code, le->what());
        }
        return unexpectedException(x);
    }


    __cold
    error error::convertCurrentException() {
        // This rigamarole recovers the current exception being thrown...
        auto xp = std::current_exception();
        if (xp) {
            try {
                std::rethrow_exception(xp);
            } catch(const std::exception& x) {
                // Now we have the exception, so we can record it in outError:
                return convertException(x);
            } catch (...) { }
        }
        auto e = error(error::LiteCore, error::UnexpectedError, "Unknown C++ exception");
        e.captureBacktrace(1);
        return e;
    }


    __cold
    bool error::isUnremarkable() const {
        if (code == 0)
            return true;
        switch (domain) {
            case LiteCore:
                return code == NotFound || code == DatabaseTooOld || code == NotOpen;
            case POSIX:
                return code == ENOENT;
            case Network:
                return code != websocket::kNetErrUnknown;
            default:
                return false;
        }
    }


    static std::function<void()> sNotableExceptionHook;

    void error::setNotableExceptionHook(function<void()> hook) {
        sNotableExceptionHook = hook;
    }


    __cold
    void error::_throw(unsigned skipFrames) {
        if (sWarnOnError && !isUnremarkable()) {
            if (sNotableExceptionHook)
                sNotableExceptionHook();
            captureBacktrace(2 + skipFrames);
            WarnError("LiteCore throwing %s error %d: %s\n%s",
                      nameOfDomain(domain), code, what(), backtrace->toString().c_str());
        }
        throw *this;
    }

    
    __cold
    void error::_throw(Domain domain, int code) {
        error{domain, code}._throw(1);
    }

    
    __cold
    void error::_throw(error::LiteCoreError err) {
        error{LiteCore, err}._throw(1);
    }

    __cold
    void error::_throwErrno() {
        error{POSIX, errno}._throw(1);
    }


    __cold
    void error::_throw(error::LiteCoreError code, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::string message = vformat(fmt, args);
        va_end(args);
        error{LiteCore, code, message}._throw(1);
    }


    __cold
    void error::_throwErrno(const char *fmt, ...) {
        int code = errno;
        va_list args;
        va_start(args, fmt);
        std::string message = vformat(fmt, args);
        va_end(args);
        message += ": ";
        message += strerror(code);
        error{POSIX, code, message}._throw(1);
    }


    __cold
    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr,
                                const char *message, ...)
    {
        string messageStr = "Assertion failed: ";
        if (message) {
            va_list args;
            va_start(args, message);
            messageStr += vformat(message, args);
            va_end(args);
        } else {
            messageStr += expr;
        }
        if (sNotableExceptionHook)
            sNotableExceptionHook();
        if (!WillLog(LogLevel::Error))
            fprintf(stderr, "%s (%s:%u, in %s)", messageStr.c_str(), file, line, fn);
        auto err = error(LiteCore, AssertionFailed, messageStr);
        err.captureBacktrace(1);     // always get backtrace of assertion failure
        WarnError("%s (%s:%u, in %s)\n%s",
                  messageStr.c_str(), file, line, fn, err.backtrace->toString().c_str());
        throw err;
    }

}

