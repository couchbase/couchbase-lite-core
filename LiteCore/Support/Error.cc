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

        char* c = buf;
        while(c) {
            if(*c == '\r') {
                *c = 0;
                break;
            }

            c++;
        }

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

    static const codeMapping kPOSIXMapping[] = {
        {EAFNOSUPPORT,                  error::POSIX,       posix::AddressFamilyNotSupported},
        {EADDRINUSE,                    error::POSIX,       posix::AddressInUse},
        {EADDRNOTAVAIL,                 error::POSIX,       posix::AddressNotAvailable},
        {EISCONN,                       error::POSIX,       posix::AlreadyConnected},
        {E2BIG,                         error::POSIX,       posix::ArgumentListTooLong},
        {EDOM,                          error::POSIX,       posix::ArgumentOutOfDomain},
        {EFAULT,                        error::POSIX,       posix::BadAddress},
        {EBADF,                         error::POSIX,       posix::BadFileDescriptor},
        {EBADMSG,                       error::POSIX,       posix::BadMessage},
        {EPIPE,                         error::POSIX,       posix::BrokenPipe},
        {ECONNABORTED,                  error::POSIX,       posix::ConnectionAborted},
        {EALREADY,                      error::POSIX,       posix::ConnectionAlreadyInProgress},
        {ECONNREFUSED,                  error::POSIX,       posix::ConnectionRefused},
        {ECONNRESET,                    error::POSIX,       posix::ConnectionReset},
        {EXDEV,                         error::POSIX,       posix::CrossDeviceLink},
        {EDESTADDRREQ,                  error::POSIX,       posix::DestinationAddressRequired},
        {EBUSY,                         error::POSIX,       posix::DeviceOrResourceBusy},
        {ENOTEMPTY,                     error::POSIX,       posix::DirectoryNotEmpty},
        {ENOEXEC,                       error::POSIX,       posix::ExecutableFormatError},
        {EEXIST,                        error::POSIX,       posix::FileExists},
        {EFBIG,                         error::POSIX,       posix::FileTooLarge},
        {ENAMETOOLONG,                  error::POSIX,       posix::FilenameTooLong},
        {ENOSYS,                        error::POSIX,       posix::FunctionNotSupported},
#ifdef EHOSTDOWN
        {EHOSTDOWN,                     error::POSIX,       posix::HostDown},
#endif
        {EHOSTUNREACH,                  error::POSIX,       posix::HostUnreachable},
        {EIDRM,                         error::POSIX,       posix::IdentifierRemoved},
        {EILSEQ,                        error::POSIX,       posix::IllegalByteSequence},
        {ENOTTY,                        error::POSIX,       posix::InappropriateIOControlOperation},
        {EINTR,                         error::POSIX,       posix::Interrupted},
        {EINVAL,                        error::POSIX,       posix::InvalidArgument},
        {ESPIPE,                        error::POSIX,       posix::InvalidSeek},
        {EIO,                           error::LiteCore,    error::IOError},
        {EISDIR,                        error::POSIX,       posix::IsADirectory},
        {EMSGSIZE,                      error::POSIX,       posix::MessageSize},
        {ENETDOWN,                      error::POSIX,       posix::NetworkDown},
        {ENETRESET,                     error::POSIX,       posix::NetworkReset},
        {ENETUNREACH,                   error::POSIX,       posix::NetworkUnreachable},
        {ENOBUFS,                       error::POSIX,       posix::NoBufferSpace},
        {ECHILD,                        error::POSIX,       posix::NoChildProcess},
        {ENOLINK,                       error::POSIX,       posix::NoLink},
#ifdef ENOLOCK
        {ENOLOCK,                       error::POSIX,       posix::NoLockAvailable},
#endif
        {ENOMSG,                        error::POSIX,       posix::NoMessage},
        {ENODATA,                       error::POSIX,       posix::NoMessageAvailable},
        {ENOPROTOOPT,                   error::POSIX,       posix::NoProtocolOption},
        {ENOSPC,                        error::POSIX,       posix::NoSpaceOnDevice},
        {ENOSR,                         error::POSIX,       posix::NoStreamResources},
        {ENODEV,                        error::POSIX,       posix::NoSuchDevice},
        {ENXIO,                         error::POSIX,       posix::NoSuchDeviceOrAddress},
        {ENOENT,                        error::LiteCore,    error::NotFound},
        {ESRCH,                         error::POSIX,       posix::NoSuchProcess},
        {ENOTDIR,                       error::POSIX,       posix::NotADirectory},
        {ENOTSOCK,                      error::POSIX,       posix::NotASocket},
        {ENOSTR,                        error::POSIX,       posix::NotAStream},
        {ENOTCONN,                      error::POSIX,       posix::NotConnected},
        {ENOMEM,                        error::POSIX,       posix::NotEnoughMemory},
#if ENOTSUP != EOPNOTSUPP
        {ENOTSUP,                       error::POSIX,       posix::NotSupported},
#endif
        {ECANCELED,                     error::POSIX,       posix::OperationCanceled},
        {EINPROGRESS,                   error::POSIX,       posix::OperationInProgress},
        {EPERM,                         error::POSIX,       posix::OperationNotPermitted},
        {EOPNOTSUPP,                    error::POSIX,       posix::OperationNotSupported},
#if EWOULDBLOCK != EAGAIN
        {EWOULDBLOCK,                   error::POSIX,       posix::OperationWouldBlock},
#endif
        {EOWNERDEAD,                    error::POSIX,       posix::OwnerDead},
        {EACCES,                        error::POSIX,       posix::PermissionDenied},
        {EPROTO,                        error::POSIX,       posix::ProtocolError},
        {EPROTONOSUPPORT,               error::POSIX,       posix::ProtocolNotSupported},
        {EROFS,                         error::POSIX,       posix::ReadOnlyFileSystem},
        {EDEADLK,                       error::POSIX,       posix::ResourceDeadlockWouldOccur},
        {EAGAIN,                        error::POSIX,       posix::ResourceUnavailableTryAgain},
        {ERANGE,                        error::POSIX,       posix::ResultOutOfRange},
        {ENOTRECOVERABLE,               error::POSIX,       posix::StateNotRecoverable},
        {ETIME,                         error::POSIX,       posix::StreamTimeout},
        {ETXTBSY,                       error::POSIX,       posix::TextFileBusy},
        {ETIMEDOUT,                     error::POSIX,       posix::TimedOut},
        {EMFILE,                        error::POSIX,       posix::TooManyFilesOpen},
        {ENFILE,                        error::POSIX,       posix::TooManyFilesOpenInSystem},
        {EMLINK,                        error::POSIX,       posix::TooManyLinks},
        {ELOOP,                         error::POSIX,       posix::TooManySymbolicLinkLevels},
        {EOVERFLOW,                     error::POSIX,       posix::ValueTooLarge},
        {EPROTOTYPE,                    error::POSIX,       posix::WrongProtocolType},

#ifdef _MSC_VER
        // Repeats for WSA specific error codes
        {WSAEADDRINUSE,                error::POSIX,        posix::AddressInUse},
        {WSAEADDRNOTAVAIL,             error::POSIX,        posix::AddressNotAvailable},
        {WSAEAFNOSUPPORT,              error::POSIX,        posix::AddressFamilyNotSupported},
        {WSAEALREADY,                  error::POSIX,        posix::ConnectionAlreadyInProgress},
        {WSAECANCELLED,                error::POSIX,        posix::OperationCanceled},
        {WSAECONNABORTED,              error::POSIX,        posix::ConnectionAborted},
        {WSAECONNREFUSED,              error::POSIX,        posix::ConnectionRefused},
        {WSAECONNRESET,                error::POSIX,        posix::ConnectionReset},
        {WSAEDESTADDRREQ,              error::POSIX,        posix::DestinationAddressRequired},
        {WSAEHOSTUNREACH,              error::POSIX,        posix::HostUnreachable},
        {WSAEINPROGRESS,               error::POSIX,        posix::OperationInProgress},
        {WSAEISCONN,                   error::POSIX,        posix::AlreadyConnected},
        {WSAELOOP,                     error::POSIX,        posix::TooManySymbolicLinkLevels},
        {WSAEMSGSIZE,                  error::POSIX,        posix::MessageSize},
        {WSAENETDOWN,                  error::POSIX,        posix::NetworkDown},
        {WSAENETRESET,                 error::POSIX,        posix::NetworkReset},
        {WSAENETUNREACH,               error::POSIX,        posix::NetworkUnreachable},
        {WSAENOBUFS,                   error::POSIX,        posix::NoBufferSpace},
        {WSAENOPROTOOPT,               error::POSIX,        posix::NoProtocolOption},
        {WSAENOTCONN,                  error::POSIX,        posix::NotConnected},
        {WSAENOTSOCK,                  error::POSIX,        posix::NotASocket},
        {WSAEOPNOTSUPP,                error::POSIX,        posix::OperationNotSupported},
        {WSAEPROTONOSUPPORT,           error::POSIX,        posix::ProtocolNotSupported},
        {WSAEPROTOTYPE,                error::POSIX,        posix::WrongProtocolType},
        {WSAETIMEDOUT,                 error::POSIX,        posix::TimedOut},
        {WSAEWOULDBLOCK,               error::POSIX,        posix::OperationWouldBlock},
#endif

        {0, /*must end with err=0*/     error::LiteCore,      0},
    };

    static const codeMapping kSQLiteMapping[] = {
        {SQLITE_PERM,                   error::LiteCore,    error::NotWriteable},
        {SQLITE_BUSY,                   error::LiteCore,    error::Busy},
        {SQLITE_LOCKED,                 error::LiteCore,    error::Busy},
        {SQLITE_NOMEM,                  error::LiteCore,    error::MemoryError},
        {SQLITE_READONLY,               error::LiteCore,    error::NotWriteable},
        {SQLITE_IOERR,                  error::LiteCore,    error::IOError},
        {SQLITE_CORRUPT,                error::LiteCore,    error::CorruptData},
        {SQLITE_FULL,                   error::POSIX,       posix::NoSpaceOnDevice},
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
            "server TLS certificate name mismatch"
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
            case POSIX: {
                for (const codeMapping *row = &kPOSIXMapping[0]; row->err != 0; ++row) {
                    if (row->code == code) {
                        return row->domain == POSIX ? cbl_strerror(row->err) : _what(row->domain, row->code);
                    }
                }
                
                return "unknown error (" + to_string(code) + ")";
            }
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
    error error::convertErrno(int err) {
        for (const codeMapping *row = &kPOSIXMapping[0]; row->err != 0; ++row) {
            if(row->err == err) {
                return error(row->domain, row->code, _what(row->domain, row->code));
            }
        }

        DebugAssert(false, "Unknown errno received in convertErrno: %d", err);
        runtime_error x("Unknown errno received: " + to_string(err));
        return unexpectedException(x);
    }


    __cold
    bool error::isUnremarkable() const {
        if (code == 0)
            return true;
        switch (domain) {
            case LiteCore:
                return code == NotFound || code == DatabaseTooOld;
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
        error::convertErrno(errno)._throw(1);
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
        error converted = error::convertErrno(errno);
        int code = converted.code;
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

