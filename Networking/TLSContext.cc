//
// TLSContext.cc
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

#include "TLSContext.hh"
#include "Certificate.hh"
#include "Logging.hh"
#include "WebSocketInterface.hh"
#include "sockpp/mbedtls_context.h"
#include <string>

using namespace std;
using namespace sockpp;
using namespace fleece;

namespace litecore { namespace net {

    TLSContext::TLSContext(role_t role)
    :_context(new mbedtls_context(role == Client ? tls_context::CLIENT : tls_context::SERVER))
    ,_role(role)
    {
        _context->set_logger(4, [=](int level, const char *filename, int line, const char *message) {
            static const LogLevel kLogLevels[] = {LogLevel::Error, LogLevel::Error,
                LogLevel::Verbose, LogLevel::Verbose, LogLevel::Debug};
            size_t len = strlen(message);
            if (message[len-1] == '\n')
                --len;
            websocket::WSLogDomain.log(kLogLevels[level], "mbedTLS(%s): %.*s",
                                       (role == Client ? "C" : "S"), int(len), message);
        });
    }

    TLSContext::~TLSContext()
    { }


    void TLSContext::setRootCerts(slice certsData) {
        _context->set_root_certs(string(certsData));
    }

    void TLSContext::setRootCerts(crypto::Cert *cert) {
        setRootCerts(cert->data());
    }

    void TLSContext::requirePeerCert(bool require) {
        _context->require_peer_cert(tls_context::role_t(_role), require);
    }

    void TLSContext::allowOnlyCert(slice certData) {
        _context->allow_only_certificate(string(certData));
    }

    void TLSContext::allowOnlyCert(crypto::Cert *cert) {
        allowOnlyCert(cert->data());
    }

    void TLSContext::setCertAuthCallback(std::function<bool(fleece::slice)> callback) {
        _context->set_auth_callback([=](const string &certData) {
            return callback(slice(certData));
        });
    }

    void TLSContext::setIdentity(crypto::Identity *id) {
        _context->set_identity(id->cert->context(), id->privateKey->context());
        _identity = id;
    }

    void TLSContext::setIdentity(slice certData, slice keyData) {
        _context->set_identity(string(certData), string(keyData));
    }



} }
