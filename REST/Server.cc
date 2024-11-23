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

    Server::Server(Delegate& delegate) : _delegate(delegate) {
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
    }

    void Server::awaitConnection() {
        lock_guard<mutex> lock(_mutex);
        if ( !_acceptor ) return;

        Poller::instance().addListener(_acceptor->handle(), Poller::kReadable, [this] {
            // Callback when a socket is accepted:
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

        //TODO: Increment/decrement _connectionCount
        _delegate.handleConnection(std::move(responder));
    }

}  // namespace litecore::REST

#endif
