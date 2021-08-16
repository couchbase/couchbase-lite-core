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
#include "TCPSocket.hh"
#include "TLSContext.hh"
#include "Poller.hh"
#include "NetworkInterfaces.hh"
#include "Certificate.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include "c4ListenerInternal.hh"
#include "PlatformCompat.hh"
#include <mutex>

// TODO: Remove these pragmas when doc-comments in sockpp are fixed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include "sockpp/tcp_acceptor.h"
#include "sockpp/inet6_address.h"
#pragma clang diagnostic pop


namespace litecore { namespace REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::net;
    using namespace sockpp;

    static bool isAnyAddress(const sock_address_any& addr) {
        if(addr.family() == AF_INET) {
            return ((const inet_address&)addr).address() == 0;
        }
        
        if(addr.family() == AF_INET6) {
            auto rawAddr = ((const inet6_address&)addr).address();
            
            // The differences in naming for each platform are annoying for this struct
            // It's 128-bits, for crying out loud so let's just see if they are all zero...
            uint64_t* ptr = (uint64_t*)&rawAddr;
            return ptr[0] == 0 && ptr[1] == 0;
        }
        
        error::_throw(error::LiteCoreError::Unimplemented);
    }

    Server::Server()
    {
        if (!ListenerLog)
            ListenerLog = c4log_getDomain("Listener", true);
    }

    
    Server::~Server() {
        stop();
    }


    uint16_t Server::port() const {
        Assert(_acceptor);
        
        // Checked that this is safe to do even for inet6_address
        // (the returned port is correct)
        inet_address ifAddr = _acceptor->address();
        return ifAddr.port();
    }


    vector<string> Server::addresses() const {
        vector<string> addresses;
        Assert(_acceptor);
        sock_address_any ifAddr = _acceptor->address();
        if (!isAnyAddress(ifAddr)) {
            // Listening on a single address:
            IPAddress listeningAddr(*ifAddr.sockaddr_ptr());
            addresses.push_back(string(listeningAddr));
        } else {
            // Not bound to any address, so it's listening on all interfaces.
            // Add the hostname if known:
            if (auto hostname = GetMyHostName(); hostname)
                addresses.push_back(*hostname);
            for (auto &addr : Interface::allAddresses()) {
                addresses.push_back(string(addr));
            }
        }
        return addresses;
    }


    static unique_ptr<sock_address> interfaceToAddress(slice networkInterface, uint16_t port) {
        if (!networkInterface)
            return make_unique<inet6_address>(port);
        // Is it an IP address?
        optional<IPAddress> addr = IPAddress::parse(string(networkInterface));
        if (!addr) {
            // Or is it an interface name?
            for (auto &intf : Interface::all()) {
                if (slice(intf.name) == networkInterface) {
                    addr = intf.primaryAddress();
                    break;
                }
            }
            if (!addr)
                throw error(error::Network, kC4NetErrUnknownHost,
                            "Unknown network interface name or address");
        }
        return addr->sockppAddress(port);
    }


    void Server::start(uint16_t port,
                       slice networkInterface,
                       TLSContext *tlsContext)
    {
        TCPSocket::initialize(); // make sure sockpp lib is initialized

        auto ifAddr = interfaceToAddress(networkInterface, port);
        _tlsContext = tlsContext;
        _acceptor.reset(new acceptor(*ifAddr));
        if (!*_acceptor)
            error::_throw(error::POSIX, _acceptor->last_error());
        _acceptor->set_non_blocking();
        c4log(ListenerLog, kC4LogInfo,"Server listening on port %d", this->port());
        awaitConnection();
    }


    void Server::stop() {
        lock_guard<mutex> lock(_mutex);
        
        // Either we never had an acceptor, or the one we tried to create
        // failed to become valid, either way don't continue
        if (!_acceptor || !*_acceptor)
            return;

        c4log(ListenerLog, kC4LogInfo,"Stopping server");
        Poller::instance().removeListeners(_acceptor->handle());
        _acceptor->close();
        _acceptor.reset();
        _rules.clear();
    }


    void Server::awaitConnection() {
        lock_guard<mutex> lock(_mutex);
        if (!_acceptor)
            return;
        
        Poller::instance().addListener(_acceptor->handle(), Poller::kReadable, [=] {
            Retained<Server> selfRetain = this;
            acceptConnection();
        });
    }


    void Server::acceptConnection() {
        try {
            // Accept a new client connection
            tcp_socket sock;
            {
                lock_guard<mutex> lock(_mutex);
                if (!_acceptor || _acceptor->is_shutdown())
                    return;
                sock = _acceptor->accept();
                if (!sock) {
                    c4log(ListenerLog, kC4LogError, "Error accepting incoming connection: %d %s",
                          _acceptor->last_error(), _acceptor->last_error_str().c_str());
                }
            }
            if (sock) {
                sock.set_non_blocking(false);
                // We are in the poller thread and go handle the client connection in a new thead to avoid
                // blocking the polling thread.
                thread handleThread([selfRetain = Retained<Server>{this}, sock = move(sock), this]() mutable {
                    this->handleConnection(move(sock));
                });
                handleThread.detach();
            }
        } catch (const std::exception &x) {
            c4log(ListenerLog, kC4LogWarning, "Caught C++ exception accepting connection: %s", x.what());
        }
        // Start another async accept:
        awaitConnection();
    }


    void Server::handleConnection(sockpp::stream_socket &&sock) {
        auto responder = make_unique<ResponderSocket>(_tlsContext);
        if (!responder->acceptSocket(move(sock)) || (_tlsContext && !responder->wrapTLS())) {
            c4log(ListenerLog, kC4LogError, "Error accepting incoming connection: %s",
                  responder->error().description().c_str());
            return;
        }
        if (c4log_willLog(ListenerLog, kC4LogVerbose)) {
            auto cert = responder->peerTLSCertificate();
            if (cert)
                c4log(ListenerLog, kC4LogVerbose, "Accepted connection from %s with TLS cert %s",
                      responder->peerAddress().c_str(), cert->subjectPublicKey()->digestString().c_str());
            else
                c4log(ListenerLog, kC4LogVerbose, "Accepted connection from %s",
                      responder->peerAddress().c_str());
        }
        RequestResponse rq(this, move(responder));
        if (rq.isValid()) {
            dispatchRequest(&rq);
            rq.finish();
        }
    }


    void Server::setExtraHeaders(const std::map<std::string, std::string> &headers) {
        lock_guard<mutex> lock(_mutex);
        _extraHeaders = headers;
    }


    void Server::addHandler(Methods methods, const string &patterns, const Handler &handler) {
        lock_guard<mutex> lock(_mutex);
        split(patterns, "|", [&](string_view pattern) {
            _rules.push_back({methods,
                              string(pattern),
                              regex(pattern.data(), pattern.size()),
                              handler});
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

        c4log(ListenerLog, kC4LogInfo, "%s %s", MethodName(method), rq->path().c_str());

        if (_authenticator) {
            if (!_authenticator(rq->header("Authorization"))) {
                c4log(ListenerLog, kC4LogInfo, "Authentication failed");
                rq->setStatus(HTTPStatus::Unauthorized, "Unauthorized");
                rq->setHeader("WWW-Authenticate", "Basic charset=\"UTF-8\"");
                return;
            }
        }

        lock_guard<mutex> lock(_mutex);

        ++_connectionCount;
        Retained<Server> retainedSelf = this;
        rq->onClose([=] { --retainedSelf->_connectionCount; });

        try {
            string pathStr(rq->path());
            auto rule = findRule(method, pathStr);
            if (rule) {
                c4log(ListenerLog, kC4LogInfo, "Matched rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                rule->handler(*rq);
            } else if (nullptr == (rule = findRule(Methods::ALL, pathStr))) {
                c4log(ListenerLog, kC4LogInfo, "No rule matched path %s", pathStr.c_str());
                rq->respondWithStatus(HTTPStatus::NotFound, "Not found");
            } else {
                c4log(ListenerLog, kC4LogInfo, "Wrong method for rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                if (method == Method::UPGRADE)
                    rq->respondWithStatus(HTTPStatus::Forbidden, "No upgrade available");
                else
                    rq->respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            }
        } catch (const std::exception &x) {
            c4log(ListenerLog, kC4LogWarning, "HTTP handler caught C++ exception: %s", x.what());
            rq->respondWithStatus(HTTPStatus::ServerError, "Internal exception");
        }
    }

} }
