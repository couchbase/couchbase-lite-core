//
// HTTPTypes.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "HTTPTypes.hh"
#include "Error.hh"

using namespace fleece;

namespace litecore { namespace net {

    static const struct {HTTPStatus code; const char* message;} kHTTPStatusMessages[] = {
        {HTTPStatus::OK,                 "OK"},
        {HTTPStatus::Created,            "Created"},
        {HTTPStatus::NoContent,          "No Content"},
        {HTTPStatus::BadRequest,         "Invalid Request"},
        {HTTPStatus::Unauthorized,       "Unauthorized"},
        {HTTPStatus::Forbidden,          "Forbidden"},
        {HTTPStatus::NotFound,           "Not Found"},
        {HTTPStatus::MethodNotAllowed,   "Method Not Allowed"},
        {HTTPStatus::NotAcceptable,      "Not Acceptable"},
        {HTTPStatus::Conflict,           "Conflict"},
        {HTTPStatus::Gone,               "Gone"},
        {HTTPStatus::PreconditionFailed, "Precondition Failed"},
        {HTTPStatus::ServerError,        "Internal Server Error"},
        {HTTPStatus::NotImplemented,     "Not Implemented"},
        {HTTPStatus::GatewayError,       "Bad Gateway"},
        {HTTPStatus::undefined,          nullptr}
    };

    const char* StatusMessage(HTTPStatus code) {
        for (unsigned i = 0; kHTTPStatusMessages[i].message; ++i) {
            if (kHTTPStatusMessages[i].code == code)
                return kHTTPStatusMessages[i].message;
        }
        return nullptr;
    }


    static constexpr size_t kNumMethods = 7;
    static const char* kMethodNames[kNumMethods] = {
        "HEAD", "GET", "PUT", "DELETE", "POST", "OPTIONS", "UPGRADE"};


    const char* MethodName(Method method) {
        int shift = -1;
        for (auto m = (unsigned)method; m != 0; m >>= 1)
            ++shift;
        if (shift < 0 || shift >= kNumMethods)
            return "??";
        return kMethodNames[shift];
    }


    Method MethodNamed(slice name) {
        for (int i = 0; i < kNumMethods; ++i) {
            if (slice(kMethodNames[i]) == name)
                return Method(1 << i);
        }
        return Method::None;
    }


    ProxySpec::ProxySpec(const C4Address &addr) {
        if (slice(addr.scheme).caseEquivalent("http"_sl))
            type = ProxyType::HTTP;
        if (slice(addr.scheme).caseEquivalent("https"_sl))
            type = ProxyType::HTTPS;
        else
            error::_throw(error::InvalidParameter, "Unknown proxy type in URL");
        hostname = addr.hostname;
        port = addr.port;
    }


    ProxySpec::operator Address () const {
        C4Address addr = {};
        static constexpr slice kProxySchemes[2] = {"http"_sl, "https"_sl};
        addr.scheme = kProxySchemes[int(type)];
        addr.hostname = hostname;
        addr.port = port;
        return Address(addr);
    }

}}
