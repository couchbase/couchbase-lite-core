//
// Server.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "Server.hh"
#include "Request.hh"
#include "XSocket.hh"
#include "Certificate.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "c4ListenerInternal.hh"
#include "PlatformCompat.hh"
#include <mutex>

// TODO: Remove these pragmas when doc-comments in sockpp are fixed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include "sockpp/tcp_acceptor.h"
#include "sockpp/mbedtls_context.h"
#pragma clang diagnostic pop


namespace litecore { namespace REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::net;
    using namespace sockpp;


    Server::Server()
    {
        stop();
    }


    void Server::start(uint16_t port, const char *hostname, crypto::Identity *identity) {
        _port = port;
        _acceptor.reset(new tcp_acceptor (port));
        if (!*_acceptor)
            error::_throw(error::POSIX, _acceptor->last_error());
        if (identity) {
            _identity = identity;
            _tlsContext.reset(new mbedtls_context(tls_context::SERVER));
            _tlsContext->set_identity(identity->cert->context(), identity->privateKey->context());
        }
        _acceptThread = thread(bind(&Server::acceptConnections, this));
    }

    
    void Server::stop() {
        if (!_acceptor)
            return;

        _acceptor->close();
        _acceptThread.join();
        _acceptor.reset();
        _rules.clear();
    }


    void Server::acceptConnections() {
        c4log(RESTLog, kC4LogInfo,"Server listening on port %d", _port);
        while (true) {
            try {
                // Accept a new client connection
                tcp_socket sock = _acceptor->accept();
                if (sock) {
                    auto responder = make_unique<XResponderSocket>(_tlsContext.get());
                    responder->acceptSocket(move(sock));
                    RequestResponse rq(this, move(responder));
                    dispatchRequest(&rq);
                    rq.finish();
                } else {
                    if (!_acceptor->is_open())
                        break;
                    c4log(RESTLog, kC4LogError, "Error accepting incoming connection: %d %s",
                          _acceptor->last_error(), _acceptor->last_error_str().c_str());
                }
            } catch (const std::exception &x) {
                c4log(RESTLog, kC4LogWarning, "Caught C++ exception accepting connection: %s", x.what());
            }
        }
        c4log(RESTLog, kC4LogInfo,"Server stopped accepting connections");
    }


    void Server::setExtraHeaders(const std::map<std::string, std::string> &headers) {
        lock_guard<mutex> lock(_mutex);
        _extraHeaders = headers;
    }


    void Server::addHandler(Methods methods, const string &patterns, const Handler &handler) {
        lock_guard<mutex> lock(_mutex);
        split(patterns, "|", [&](const string &pattern) {
            _rules.push_back({methods, pattern, regex(pattern.c_str()), handler});
        });
    }


    Server::URIRule* Server::findRule(Method method, const string &path) {
        //lock_guard<mutex> lock(_mutex);       // called from dispatchResponder which locks
        for (auto &rule : _rules) {
            if ((rule.methods & method)
                    && regex_match(path.c_str(), rule.regex))
                return &rule;
        }
        return nullptr;
    }


    void Server::dispatchRequest(RequestResponse *rq) {
        Method method = rq->method();
        if (method == Method::GET && rq->header("Connection") == "Upgrade"_sl)
            method = Method::UPGRADE;

        c4log(RESTLog, kC4LogInfo, "%s %s", MethodName(method), rq->path().c_str());
        lock_guard<mutex> lock(_mutex);
        try{
            string pathStr(rq->path());
            auto rule = findRule(method, pathStr);
            if (rule) {
                c4log(RESTLog, kC4LogInfo, "Matched rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                rule->handler(*rq);
            } else if (nullptr == (rule = findRule(Methods::ALL, pathStr))) {
                c4log(RESTLog, kC4LogInfo, "No rule matched path %s", pathStr.c_str());
                rq->respondWithStatus(HTTPStatus::NotFound, "Not found");
            } else {
                c4log(RESTLog, kC4LogInfo, "Wrong method for rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                if (method == Method::UPGRADE)
                    rq->respondWithStatus(HTTPStatus::Forbidden, "No upgrade available");
                else
                    rq->respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            }
        } catch (const std::exception &x) {
            c4log(RESTLog, kC4LogWarning, "HTTP handler caught C++ exception: %s", x.what());
            rq->respondWithStatus(HTTPStatus::ServerError, "Internal exception");
        }
    }

} }
