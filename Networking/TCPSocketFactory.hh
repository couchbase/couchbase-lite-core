//
// TCPSocketFactory.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Socket.hh"
#include "Logging.hh"
#include "RingBuffer.hh"
#include "StringUtil.hh"
#include "TCPSocket.hh"
#include "WebSocketInterface.hh"
#include <mutex>
#include <string>

namespace litecore::net {

    /** A socket factory that uses TCPSocket to implement TCP connections.
     *  Currently, this is only used by tests (SocketFactoryTest.cc), not the LiteCore library. */
    class TCPSocketFactory final : public C4SocketFactoryImpl, Logging {
    public:
        /// Constructor for a client socket (to be created when `open` is called.)
        TCPSocketFactory()
        :Logging(websocket::WSLogDomain)
        { }

        /// Constructor for a server-side socket.
        explicit TCPSocketFactory(std::unique_ptr<ResponderSocket> responderSocket)
        :Logging(websocket::WSLogDomain)
        ,_tcpSocket{std::move(responderSocket)}
        { }

        ~TCPSocketFactory() override {
            logDebug("~TCPSocketFactory");
        }

    protected:
        // C4SocketFactory methods:

        void open(C4Socket* socket, C4Address const& address, C4Slice options) override {
            std::unique_lock lock(_mutex);
            opened(socket);
            _identifier = stringprintf("%.*s:%d", FMTSLICE(address.hostname), address.port);
            logInfo("Opening on %s ...", _identifier.c_str());

            assert_precondition(!_tcpSocket);
            TCPSocket::initialize();
            auto clientSocket = std::make_unique<ClientSocket>();
            if (clientSocket->connect(Address(address))) {
                _tcpSocket = std::move(clientSocket);
                openComplete();
            } else {
                closeWithError();
            }
        }

        void attached() override {
            C4SocketFactoryImpl::attached();
            if (_tcpSocket)
                openComplete();
        }

        void write(alloc_slice data) override {
            if (data.size > 0) {
                std::unique_lock lock(_mutex);
                logDebug("Client gave me %zu bytes to write", data.size);
                if (_writeBuffer.empty())
                    awaitWriteable();
                _writeBuffer.growAndWrite(data);
            }
        }

        void completedReceive(size_t byteCount) override {
            if (byteCount > 0) {
                std::unique_lock lock(_mutex);
                logDebug("Client completed reading %zu bytes", byteCount);
                Assert(byteCount + _curReadCapacity <= kReadBufferSize);
                if (_curReadCapacity == 0)
                    awaitReadable();
                _curReadCapacity += byteCount;
            }
        }

        void close() override {
            std::unique_lock lock(_mutex);
            logDebug("Client closing");
            if (_tcpSocket)
                _tcpSocket->close();
        }

    private:
        std::string loggingIdentifier() const override { return _identifier; }

        void openComplete() {
            logVerbose("...open completed");
            assert_precondition(_tcpSocket);
            _selfRetain = this;
            _tcpSocket->setNonBlocking(true);
            _tcpSocket->onDisconnect([this] {disconnected();});
            awaitReadable();
            socket()->opened();
        }

        void awaitReadable() {
            _tcpSocket->onReadable([this] { readFromSocket(); });
        }

        void readFromSocket() {
            std::unique_lock lock(_mutex);
            if ( !_tcpSocket->connected() ) {
                // close() has been called:
                logDebug("readFromSocket: disconnected");
                closeWithError();
                return;
            }

            ssize_t n = _tcpSocket->read(_readBuffer.data(), std::min(_readBuffer.size(), _curReadCapacity));
            if (n > 0) {
                // The bytes read count against the read-capacity:
                _curReadCapacity -= n;
                logVerbose("Read %zd bytes", n);
                if (_curReadCapacity > 0 ) awaitReadable();
                socket()->received(slice(_readBuffer.data(), n));
            } else if (n == 0) {
                if ( _tcpSocket->atReadEOF() ) {
                    logVerbose("Zero-byte read: EOF from peer");
                    socket()->received(nullslice);
                } else {
                    logDebug("**** socket got EWOULDLOCK");
                    awaitReadable();
                }
            } else {
                closeWithError();
            }
        }

        void awaitWriteable() {
            _tcpSocket->onWriteable([this] {this->writeToSocket();});
        }

        void writeToSocket() {
            std::unique_lock lock(_mutex);
            while (!_writeBuffer.empty()) {
                slice chunk = _writeBuffer.peek();
                ssize_t n = _tcpSocket->write(chunk);
                if (n >= 0) {
                    logDebug("Sent %zd of %zu bytes", n, chunk.size);
                    _writeBuffer.discard(n);
                    socket()->completedWrite(n);
                    if (n < chunk.size) {
                        awaitWriteable();
                        break;
                    }
                } else {
                    closeWithError();
                    break;
                }
            }
        }

        void disconnected() {
            std::unique_lock lock(_mutex);
            logVerbose("Disconnected");
            closeWithError();
        }

        void closeWithError() {
            C4Error error = _tcpSocket->error();
            if (!error)
                logInfo("Closed");
            else if (error == C4Error{POSIXDomain, ECONNRESET} && dynamic_cast<ResponderSocket*>(_tcpSocket.get()))
                logInfo("Closed by client (ECONNRESET)");
            else
                logError("Closed with error %s", error.description().c_str());
            _tcpSocket->cancelCallbacks();
            socket()->closed(error);
            releaseSocket();
            _selfRetain = nullptr;  // allow myself to be freed now
        }


        static constexpr size_t kReadBufferSize         = 32 * 1024;
        static constexpr size_t kWriteBufferInitialSize = 32 * 1024;

        std::recursive_mutex            _mutex;
        Retained<TCPSocketFactory>      _selfRetain;
        std::string                     _identifier;
        std::unique_ptr<TCPSocket>      _tcpSocket;
        RingBuffer                      _writeBuffer {kWriteBufferInitialSize};
        size_t                          _curReadCapacity = kReadBufferSize;
        std::array<std::byte,kReadBufferSize> _readBuffer;
    };

}
