//
// HTTPTypes.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "HTTPTypes.hh"
#include "Error.hh"

using namespace fleece;

namespace litecore::net {

    static const struct {
        HTTPStatus  code;
        const char* message;
    } kHTTPStatusMessages[] = {{HTTPStatus::OK, "OK"},
                               {HTTPStatus::Created, "Created"},
                               {HTTPStatus::NoContent, "No Content"},
                               {HTTPStatus::BadRequest, "Invalid Request"},
                               {HTTPStatus::Unauthorized, "Unauthorized"},
                               {HTTPStatus::Forbidden, "Forbidden"},
                               {HTTPStatus::NotFound, "Not Found"},
                               {HTTPStatus::MethodNotAllowed, "Method Not Allowed"},
                               {HTTPStatus::NotAcceptable, "Not Acceptable"},
                               {HTTPStatus::Conflict, "Conflict"},
                               {HTTPStatus::Gone, "Gone"},
                               {HTTPStatus::PreconditionFailed, "Precondition Failed"},
                               {HTTPStatus::ServerError, "Internal Server Error"},
                               {HTTPStatus::NotImplemented, "Not Implemented"},
                               {HTTPStatus::GatewayError, "Bad Gateway"},
                               {HTTPStatus::undefined, nullptr}};

    const char* StatusMessage(HTTPStatus code) {
        for ( unsigned i = 0; kHTTPStatusMessages[i].message; ++i ) {
            if ( kHTTPStatusMessages[i].code == code ) return kHTTPStatusMessages[i].message;
        }
        return nullptr;
    }

    static constexpr size_t kNumMethods               = 7;
    static const char*      kMethodNames[kNumMethods] = {"HEAD", "GET", "PUT", "DELETE", "POST", "OPTIONS", "UPGRADE"};

    const char* MethodName(Method method) {
        int shift = -1;
        for ( auto m = (unsigned)method; m != 0; m >>= 1 ) ++shift;
        if ( shift < 0 || shift >= kNumMethods ) return "??";
        return kMethodNames[shift];
    }

    Method MethodNamed(slice name) {
        for ( int i = 0; i < kNumMethods; ++i ) {
            if ( slice(kMethodNames[i]) == name ) return Method(1 << i);
        }
        return Method::None;
    }

    HTTPStatus StatusFromError(C4Error err) {
        if ( err.code == 0 ) return HTTPStatus::OK;
        HTTPStatus status = HTTPStatus::ServerError;
        // TODO: Add more mappings, and make these table-driven
        switch ( err.domain ) {
            case LiteCoreDomain:
                switch ( err.code ) {
                    case kC4ErrorInvalidParameter:
                    case kC4ErrorBadRevisionID:
                        status = HTTPStatus::BadRequest;
                        break;
                    case kC4ErrorNotADatabaseFile:
                    case kC4ErrorCrypto:
                        status = HTTPStatus::Unauthorized;
                        break;
                    case kC4ErrorNotWriteable:
                        status = HTTPStatus::Forbidden;
                        break;
                    case kC4ErrorNotFound:
                        status = HTTPStatus::NotFound;
                        break;
                    case kC4ErrorConflict:
                        status = HTTPStatus::Conflict;
                        break;
                    case kC4ErrorUnimplemented:
                    case kC4ErrorUnsupported:
                        status = HTTPStatus::NotImplemented;
                        break;
                    case kC4ErrorRemoteError:
                        status = HTTPStatus::GatewayError;
                        break;
                    case kC4ErrorBusy:
                        status = HTTPStatus::Locked;
                        break;
                }
                break;
            case WebSocketDomain:
                if ( err.code < 1000 ) status = HTTPStatus(err.code);
            default:
                break;
        }
        return status;
    }

    ProxySpec::ProxySpec(const C4Address& addr) {
        if ( slice(addr.scheme).caseEquivalent("http"_sl) ) type = ProxyType::HTTP;
        if ( slice(addr.scheme).caseEquivalent("https"_sl) ) type = ProxyType::HTTPS;
        else
            error::_throw(error::InvalidParameter, "Unknown proxy type in URL");
        hostname = addr.hostname;
        port     = addr.port;
    }

    ProxySpec::operator Address() const {
        C4Address              addr             = {};
        static constexpr slice kProxySchemes[2] = {"http"_sl, "https"_sl};
        addr.scheme                             = kProxySchemes[int(type)];
        addr.hostname                           = hostname;
        addr.port                               = port;
        return Address(addr);
    }

}  // namespace litecore::net
