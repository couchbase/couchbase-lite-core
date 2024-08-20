//
// Server.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
#include "c4ListenerInternal.hh"
#include <memory>
#include <mutex>

#ifdef COUCHBASE_ENTERPRISE

// TODO: Remove these pragmas when doc-comments in sockpp are fixed
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdocumentation"
#    include "sockpp/tcp_acceptor.h"
#    include "sockpp/inet6_address.h"
#    pragma clang diagnostic pop

namespace litecore::REST {
    using namespace std;
    using namespace fleece;
    using namespace litecore::net;
    using namespace sockpp;

    static bool isAnyAddress(const sock_address_any& addr) {
        if ( addr.family() == AF_INET ) { return ((const inet_address&)addr).address() == 0; }

        if ( addr.family() == AF_INET6 ) {
            auto rawAddr = ((const inet6_address&)addr).address();

            // The differences in naming for each platform are annoying for this struct
            // It's 128-bits, for crying out loud so let's just see if they are all zero...
            auto* ptr = (uint64_t*)&rawAddr;
            return ptr[0] == 0 && ptr[1] == 0;
        }

        error::_throw(error::LiteCoreError::Unimplemented);
    }

    Server::Server() {
        if ( !ListenerLog ) ListenerLog = c4log_getDomain("Listener", true);
        if ( !RESTLog ) RESTLog = c4log_getDomain("REST", true);
    }

    Server::~Server() { stop(); }

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
        if ( !isAnyAddress(ifAddr) ) {
            // Listening on a single address:
            IPAddress listeningAddr(*ifAddr.sockaddr_ptr());
            addresses.push_back(string(listeningAddr));
        } else {
            // Not bound to any address, so it's listening on all interfaces.
            // Add the hostname if known:
            if ( auto hostname = GetMyHostName(); hostname ) addresses.push_back(*hostname);
            for ( auto& addr : Interface::allAddresses() ) { addresses.push_back(string(addr)); }
        }
        return addresses;
    }

    static unique_ptr<sock_address> interfaceToAddress(slice networkInterface, uint16_t port) {
        if ( !networkInterface ) return make_unique<inet6_address>(port);
        // Is it an IP address?
        optional<IPAddress> addr = IPAddress::parse(string(networkInterface));
        if ( !addr ) {
            // Or is it an interface name?
            for ( auto& intf : Interface::all() ) {
                if ( slice(intf.name) == networkInterface ) {
                    addr = intf.primaryAddress();
                    break;
                }
            }
            if ( !addr ) throw error(error::Network, kC4NetErrUnknownHost, "Unknown network interface name or address");
        }
        return addr->sockppAddress(port);
    }

    void Server::start(uint16_t port, slice networkInterface, TLSContext* tlsContext) {
        TCPSocket::initialize();  // make sure sockpp lib is initialized

        auto ifAddr = interfaceToAddress(networkInterface, port);
        _tlsContext = tlsContext;
        _acceptor   = std::make_unique<acceptor>(*ifAddr);
        if ( !*_acceptor ) error::_throw(error::POSIX, _acceptor->last_error());
        _acceptor->set_non_blocking();
        c4log(ListenerLog, kC4LogInfo, "Server listening on port %d", this->port());
        awaitConnection();
    }

    void Server::stop() {
        lock_guard<mutex> lock(_mutex);

        // Either we never had an acceptor, or the one we tried to create
        // failed to become valid, either way don't continue
        if ( !_acceptor || !*_acceptor ) return;

        c4log(ListenerLog, kC4LogInfo, "Stopping server");
        Poller::instance().removeListeners(_acceptor->handle());
        _acceptor->close();
        _acceptor.reset();
        _rules.clear();
    }

    void Server::awaitConnection() {
        lock_guard<mutex> lock(_mutex);
        if ( !_acceptor ) return;

        Poller::instance().addListener(_acceptor->handle(), Poller::kReadable, [this] {
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
                if ( !_acceptor || _acceptor->is_shutdown() ) return;
                sock = _acceptor->accept();
                if ( !sock ) {
                    c4log(ListenerLog, kC4LogError, "Error accepting incoming connection: %d %s",
                          _acceptor->last_error(), _acceptor->last_error_str().c_str());
                }
            }
            if ( sock ) {
                sock.set_non_blocking(false);
                // We are in the poller thread and go handle the client connection in a new thead to avoid
                // blocking the polling thread.
                thread handleThread([selfRetain = Retained<Server>{this}, sock = std::move(sock), this]() mutable {
                    this->handleConnection(std::move(sock));
                });
                handleThread.detach();
            }
        } catch ( const std::exception& x ) {
            c4log(ListenerLog, kC4LogWarning, "Caught C++ exception accepting connection: %s", x.what());
        }
        // Start another async accept:
        awaitConnection();
    }

    void Server::handleConnection(sockpp::stream_socket&& sock) {
        auto responder = make_unique<ResponderSocket>(_tlsContext);
        if ( !responder->acceptSocket(std::move(sock)) || (_tlsContext && !responder->wrapTLS()) ) {
            C4Error error       = responder->error();
            string  description = error.description();
            if ( error.domain == NetworkDomain ) {
                // The default messages call the peer "server" and me "client"; reverse that:
                replace(description, "server", "CLIENT");
                replace(description, "client", "server");
                replace(description, "CLIENT", "client");
            }
            c4log(ListenerLog, kC4LogError, "Error accepting incoming connection: %s", description.c_str());
            return;
        }

        string peer             = responder->peerAddress();
        bool   loggedConnection = false;
        if ( c4log_willLog(ListenerLog, kC4LogVerbose) ) {
            if ( auto cert = responder->peerTLSCertificate() ) {
                c4log(ListenerLog, kC4LogVerbose, "Accepted connection from %s with TLS cert %s",
                      responder->peerAddress().c_str(), cert->subjectPublicKey()->digestString().c_str());
                loggedConnection = true;
            }
        }
        if ( !loggedConnection ) c4log(ListenerLog, kC4LogInfo, "Accepted connection from %s", peer.c_str());

        // Now read one or more requests and write responses:
        while ( true ) {
            // Read HTTP request from socket:
            RequestResponse rq(std::move(responder));
            if ( C4Error err = rq.socketError() ) {
                if ( err == C4Error{NetworkDomain, kC4NetErrConnectionReset} ) {
                    c4log(ListenerLog, kC4LogInfo, "End of socket connection from %s (closed by peer)", peer.c_str());
                } else {
                    c4log(ListenerLog, kC4LogError, "Error reading HTTP request from %s: %s", peer.c_str(),
                          err.description().c_str());
                }
                break;
            }
            rq.addHeaders(_extraHeaders);
            auto   method    = rq.method();
            string uri       = rq.uri();  // save it now, as it may be cleared if rq gets moved
            bool   keepAlive = rq.keepAlive();
            if ( keepAlive && rq.httpVersion() == Request::HTTP1_0 ) rq.setHeader("Connection", "keep-alive");

            // Handle it!
            dispatchRequest(rq);
            c4log(RESTLog, kC4LogInfo, "%s\t%s\t%s\t-> %d", peer.c_str(), MethodName(method), uri.c_str(), rq.status());

            // Either close, or take back the socket:
            if ( !keepAlive || rq.responseHeaders()["Connection"] == "close" ) {
                c4log(ListenerLog, kC4LogInfo, "End of socket connection from %s (Connection:close)", peer.c_str());
                break;
            }
            responder = rq.extractSocket();  // Get the socket back, unless it's been given to a WebSocket
            if ( !responder ) { break; }
        }
    }

    void Server::setExtraHeaders(const std::map<std::string, std::string>& headers) {
        lock_guard<mutex> lock(_mutex);
        _extraHeaders = headers;
    }

    void Server::addHandler(Methods methods, string_view pattern, APIVersion version, Handler handler) {
        precondition(handler);
        lock_guard<mutex> lock(_mutex);
        _rules.push_back(
                {.methods = methods, .pattern = string(pattern), .version = version, .handler = std::move(handler)});
    }

    Server::URIRule* Server::findRule(Method method, const string& path) {
        // Convert the request path to a pattern:
        string pattern = "";
        split(path, "/", [&](string_view component) {
            if ( !component.empty() ) {
                pattern += '/';
                if ( component[0] == '_' ) pattern += component;
                else
                    pattern += '*';
            }
        });
        if ( pattern.empty() ) pattern = "/";

        if ( method == Methods::HEAD ) method = Methods::GET;

        // Now look up the pattern:
        lock_guard<mutex> lock(_mutex);
        for ( auto& rule : _rules ) {
            if ( (rule.methods & method) && rule.pattern == pattern ) return &rule;
        }
        return nullptr;
    }

    void Server::dispatchRequest(RequestResponse& rq) {
        try {
            Method method = rq.method();
            if ( method == Method::GET && rq.header("Connection") == "Upgrade"_sl ) method = Method::UPGRADE;

            if ( !_authenticator || _authenticator(rq.header("Authorization")) ) {
                ++_connectionCount;
                Retained<Server> retainedSelf = this;
                rq.onClose([=] { --retainedSelf->_connectionCount; });

                string pathStr(rq.path());
                auto   rule = findRule(method, pathStr);
                if ( rule ) {
                    // Found a rule; check the version:
                    c4log(ListenerLog, kC4LogVerbose, "Matched rule %s for path %s", rule->pattern.c_str(),
                          pathStr.c_str());
                    APIVersion rqVers = APIVersion::parse(rq.header("API-Version"));
                    if ( rqVers.major < rule->version.major )
                        rq.respondWithStatus(HTTPStatus::BadRequest, "Version too old");
                    else if ( rqVers.major > rule->version.major )
                        rq.respondWithStatus(HTTPStatus::BadRequest, "Version too new");
                    else
                        rule->handler(rq);  // Dispatch request to handler method!
                } else if ( nullptr == (rule = findRule(Methods::ALL, pathStr)) ) {
                    // No such rule:
                    c4log(ListenerLog, kC4LogInfo, "No rule matched path %s", pathStr.c_str());
                    rq.respondWithStatus(HTTPStatus::NotFound, "Not found");
                } else {
                    // Wrong HTTP method:
                    c4log(ListenerLog, kC4LogInfo, "Wrong method for rule %s for path %s", rule->pattern.c_str(),
                          pathStr.c_str());
                    if ( method == Method::UPGRADE )
                        rq.respondWithStatus(HTTPStatus::Forbidden, "No upgrade available");
                    else
                        rq.respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
                }
            } else {
                c4log(ListenerLog, kC4LogInfo, "Authentication failed");
                rq.setStatus(HTTPStatus::Unauthorized, "Unauthorized");
                rq.setHeader("WWW-Authenticate", "Basic charset=\"UTF-8\"");
            }
        } catch ( const std::exception& ) {
            c4log(ListenerLog, kC4LogWarning, "HTTP handler caught C++ exception: %s",
                  C4Error::fromCurrentException().description().c_str());
            if ( !rq.finished() ) rq.respondWithStatus(HTTPStatus::ServerError, "Internal exception");
        }
        rq.finish();
    }

    Server::APIVersion Server::APIVersion::parse(string_view str) {
        APIVersion vers{1, 0};
        if ( !str.empty() ) { (void)sscanf(string(str).c_str(), "%hhu.%hhu", &vers.major, &vers.minor); }
        return vers;
    }

}  // namespace litecore::REST

#endif
