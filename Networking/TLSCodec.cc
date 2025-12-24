//
// TLSCodec.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "TLSCodec.hh"
#include "Address.hh"
#include "c4Socket.hh"
#include "RingBuffer.hh"
#include "TLSContext.hh"
#include "WebSocketInterface.hh"
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <sockpp/mbedtls_context.h>
#include <mutex>
#include <utility>

namespace litecore::net {
    using namespace std;

    /** A combination C4Socket & C4SocketFactory that adds TLS to an underlying C4SocketFactory. */
    class TLSSocket final
        : public C4SocketFactoryImpl
        , public C4Socket
        , public Logging {
      public:
        /** Constructor.
         * @param platformFactory  The underlying socket factory to be wrapped.
         * @param platformNativeHandle  An existing `nativeHandle`, if any, for the platformFactory.
         * @param tlsContext  The TLS context to use. */
        TLSSocket(const C4SocketFactory& platformFactory, void* platformNativeHandle, TLSContext* tlsContext)
            : C4Socket(platformFactory, platformNativeHandle)
            , Logging(websocket::WSLogDomain)
            , _tlsContext(tlsContext)
            , _cleartextSendBuffer{kBufferSize} {
            Assert(platformNativeHandle != nullptr || platformFactory.context != nullptr);
        }

        ~TLSSocket() override {
            logDebug("~TLSSocket");
            if ( _ssl ) mbedtls_ssl_free(_ssl.get());
        }

        std::string_view hostname() const {
            if ( !_url.empty() ) {
                C4Address addr;
                if ( C4Address::fromURL(_url, &addr, nullptr) ) return slice(addr.hostname);
            }
            return "";
        }

        //-------- C4Socket API -- called by the "downstream" platform's socket-factory protocol code

        // Note: The platform code will never call these methods concurrently.

        /// Downstream platform socket opened.
        void opened() override {
            BIO::Result ioResult;
            {
                unique_lock lock(_mutex);
                initTLS();
                processData();
                ioResult = getBioResult();
            }
            performBIO(ioResult);
        }

        /// Downstream platform socket closed.
        void closed(C4Error errorIfAny) override {
            unique_lock lock(_mutex);
            if ( errorIfAny ) setError(errorIfAny);
            if ( _state != State::closed ) {
                if ( _error ) logError("Closed, with error %s", _error.description().c_str());
                else
                    logInfo("Closed");
                _state = State::closed;
                _bio.closed();
                socket()->closed(_error);
            }
            releaseSocket();
        }

        void closeRequested(int status, slice message) override {
            error::_throw(error::Unimplemented);  // should never be called
        }

        /// Downstream platform socket consumed ciphertext I sent it.
        void completedWrite(size_t byteCount) override {
            bool closeNow;
            {
                unique_lock lock(_mutex);
                Assert(byteCount <= _pendingDownstreamWrites);
                _pendingDownstreamWrites -= byteCount;
                //TODO: Flow control
                closeNow = (_state == State::closing && _pendingDownstreamWrites == 0);
            }
            if ( closeNow ) _factory.close(this);
        }

        /// Downstream platform socket received ciphertext data.
        void received(slice data) override {
            BIO::Result ioResult;
            {
                unique_lock lock(_mutex);
                if ( _state < State::closing ) {
                    _bio.addToRecvBuffer(data);
                    processData();
                    ioResult = getBioResult();
                }
            }
            performBIO(ioResult);
        }

        bool gotPeerCertificate(slice certData, string_view hostname) override {
            error::_throw(error::Unimplemented);  // should never be called
        }

        void gotHTTPResponse(int httpStatus, slice responseHeadersFleece) override {
            error::_throw(error::Unimplemented);  // should never be called
        }

      protected:
        std::string loggingIdentifier() const override { return _url; }

        //-------- My C4SocketFactory's "methods"; called by upstream C4WebSocket


        /// Upstream WebSocket wants to open a connection.
        void open(C4Socket* socket, const C4Address& addr, C4Slice options) override {
            {
                unique_lock lock(_mutex);
                C4SocketFactoryImpl::opened(socket);
                _url = Address(addr).url();
                logInfo("Connecting to %.*s:%d ...", FMTSLICE(addr.hostname), addr.port);
            }

            // Delegate the call downstream, but convert the URL scheme to 'http' so the platform
            // factory doesn't think it's supposed to handle TLS itself:
            C4Address plainAddr = addr;
            if ( slice(plainAddr.scheme).hasSuffix('s') ) --plainAddr.scheme.size;
            if ( plainAddr.port == 0 ) plainAddr.port = 443;

            Assert(_factory.context);
            _factory.open(this, &plainAddr, options, _factory.context);
        }

        /// Upstream WebSocket has attached to me.
        void attached() override {
            if ( _factory.attached ) _factory.attached(this);
        }

        /// Upstream WebSocket wants to send (cleartext) data.
        void write(alloc_slice data) override {
            BIO::Result ioResult;
            {
                unique_lock lock(_mutex);
                if ( _state < State::closing ) {
                    _cleartextSendBuffer.growAndWrite(data);
                    processData();
                    ioResult = getBioResult();
                }
            }
            performBIO(ioResult);
        }

        /// Upstream WebSocket has processed data I sent it.
        void completedReceive(size_t byteCount) override {
            unique_lock lock(_mutex);
            Assert(byteCount <= _pendingUpstreamReceived);
            _pendingUpstreamReceived -= byteCount;
            //TODO: Flow control
        }

        /// Upstream WebSocket wants to close the connection.
        void close() override {
            logInfo("Close requested");
            BIO::Result ioResult{};
            bool        closeNow = false;
            {
                unique_lock lock(_mutex);
                if ( _state < State::closing ) {
                    if ( auto ssl = _ssl.get() ) {
                        check(mbedtls_ssl_close_notify(ssl));
                        ioResult = getBioResult();
                        if ( _pendingDownstreamWrites == 0 && ioResult.toWrite.empty() ) closeNow = true;
                    } else {
                        closeNow = true;
                    }

                    if ( closeNow ) {
                        _state = State::closed;
                        _bio.closed();
                    } else {
                        _state = State::closing;
                    }
                }
            }
            performBIO(ioResult);
            if ( closeNow ) _factory.close(this);
        }

        void socket_retain() override { fleece::retain(this); }

        void socket_release() override { fleece::release(this); }

      private:
        /// Set up my `mbedtls_ssl_context` when the platform socket opens.
        bool initTLS() {
            logDebug("initializing TLS, waiting for handshake");
            assert_precondition(!_ssl);
            _ssl = make_unique<mbedtls_ssl_context>();
            mbedtls_ssl_init(_ssl.get());

            auto& context = _tlsContext->get_mbedtls_context();
            if ( !check(context.status()) ) return false;
            if ( !check(mbedtls_ssl_setup(_ssl.get(), context.get_ssl_config())) ) return false;
            if ( auto host = hostname(); !host.empty() ) {
                if ( !check(mbedtls_ssl_set_hostname(_ssl.get(), string(host).c_str())) ) return false;
            }

            _bio.initSSLCallbacks(_ssl.get());
            _state = State::handshake;
            return true;
        }

        /// Let mbedTLS do its thing.
        void processData() {
            if ( _state == State::handshake ) {
                // During the handshake, let mbedTLS send/receive ciphertext:
                logDebug("Processing handshake...");
                if ( int status = mbedtls_ssl_handshake(_ssl.get()); status == 0 ) {
                    logVerbose("TLS handshake complete");
                    if ( !verifyPeer() ) return;
                    // on successful handshake, continue...
                } else {
                    check(status);
                    return;
                }
            } else if ( _state >= State::closing ) {
                return;
            }

            // After the handshake, push/pull cleartext data:
            logDebug("Processing data...");
            bool madeProgress;
            do {
                Assert(_state == State::open);
                madeProgress = false;
                if ( !_cleartextSendBuffer.empty() ) {
                    // Push available upstream cleartext (added by `write()`) into mbedTLS.
                    slice data = _cleartextSendBuffer.peek();
                    int   n    = mbedtls_ssl_write(_ssl.get(), data.begin(), data.size);
                    if ( n > 0 ) {
                        logDebug("Encrypted %d of %zu bytes", n, data.size);
                        (void)_cleartextSendBuffer.readSome(n);
                        socket()->completedWrite(n);
                        madeProgress = true;
                    } else if ( !check(n) ) {
                        return;
                    }
                }
                {
                    // Pull any available cleartext out of mbedTLS and send it upstream:
                    int n;
                    if ( !_cleartextRecvBuffer ) _cleartextRecvBuffer = make_unique<uint8_t[]>(kBufferSize);
                    while ( (n = mbedtls_ssl_read(_ssl.get(), _cleartextRecvBuffer.get(), kBufferSize)) > 0 ) {
                        logDebug("Decrypted %d bytes", n);
                        _pendingUpstreamReceived += n;  //TODO: Flow control
                        socket()->received(slice(_cleartextRecvBuffer.get(), n));
                        madeProgress = true;
                    }
                    if ( n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ) {
                        logDebug("Peer sent EOF");
                        socket()->received(nullslice);
                        madeProgress = true;
                    } else if ( !check(n) ) {
                        return;
                    }
                }
            } while ( madeProgress );
        }

        // After handshake completes, verifies the peer's certificate.
        bool verifyPeer() {
            uint32_t verify_flags = mbedtls_ssl_get_verify_result(_ssl.get());
            if ( verify_flags != 0 && verify_flags != 0xFFFFFFFF
                 && !(verify_flags & MBEDTLS_X509_BADCERT_SKIP_VERIFY) ) {
                char vrfy_buf[512];
                mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "", verify_flags);
                warn("Cert verify failed: %s", vrfy_buf);
                return check(MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
            }

            if ( mbedtls_x509_crt const* cert = mbedtls_ssl_get_peer_cert(_ssl.get()) ) {
                if ( !socket()->gotPeerCertificate(slice(cert->raw.p, cert->raw.len), hostname()) ) {
                    warn("Peer cert was rejected by app");
                    return check(MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
                }
            }

            logInfo("Socket is open!");
            _state = State::open;
            socket()->opened();
            return true;
        }

        // -------- mbedTLS BIO callbacks

        // These are called by:
        // mbedtls_ssl_handshake, mbedtls_ssl_write, mbedtls_ssl_read, mbedtls_ssl_close_notify


        /** Object that captures the (ciphertext) I/O performed by mbedTLS. */
        struct BIO {
            /// Points the SSL context's BIO send/recv callbacks to my own methods.
            void initSSLCallbacks(mbedtls_ssl_context* ssl) {
                mbedtls_ssl_send_t* send = [](void* ctx, const uint8_t* buf, size_t len) {
                    return ((BIO*)ctx)->send(buf, len);
                };
                mbedtls_ssl_recv_t* recv = [](void* ctx, uint8_t* buf, size_t len) {
                    return ((BIO*)ctx)->recv(buf, len);
                };
                mbedtls_ssl_set_bio(ssl, this, send, recv, nullptr);
            }

            /// Adds incoming ciphertext to my recvBuffer.
            void addToRecvBuffer(slice data) {
                if ( data.empty() ) _readEOF = true;
                else
                    _recvBuffer.growAndWrite(data);
            }

            /// Called when the socket is completely closed.
            void closed() { _closed = true; }

            struct Result {
                size_t      bytesRead = 0;
                alloc_slice toWrite;
            };

            /// Returns the bytes to send, and the number of bytes received, since the last call.
            [[nodiscard]] Result getResult() {
                Result result;
                if ( !_closed ) {
                    result.bytesRead = _bytesRcvd;
                    _bytesRcvd       = 0;
                    if ( !_sendBuffer.empty() ) {
                        result.toWrite = alloc_slice(_sendBuffer);
                        _sendBuffer.clear();
                    }
                }
                return result;
            }

          private:
            int recv(void* buf, size_t length) {
                LogDebug(websocket::WSLogDomain, "mbedTLS wants to read %zu bytes; %zu available", length,
                         _recvBuffer.size());
                if ( _closed ) return MBEDTLS_ERR_NET_CONN_RESET;
                if ( _recvBuffer.empty() ) return _readEOF ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
                size_t nRead = _recvBuffer.read(buf, length);
                _bytesRcvd += nRead;
                return int(nRead);
            }

            int send(const void* buf, size_t length) {
                LogDebug(websocket::WSLogDomain, "mbedTLS wants to write %zu bytes", length);
                if ( _closed ) return MBEDTLS_ERR_NET_CONN_RESET;
                _sendBuffer.append((const char*)buf, length);
                return int(length);
            }

            RingBuffer _recvBuffer{kBufferSize};  // Bytes to be consumed by recv()
            size_t     _bytesRcvd = 0;            // Number of bytes consumed by recv() this time
            string     _sendBuffer;               // Bytes produced by send()
            bool       _closed  = false;          // Set to true when socket closes
            bool       _readEOF = false;          // True if recv side has reached EOF
        };

        /// Extracts the results of the BIO calls into a struct.
        /// My _mutex must be locked.
        [[nodiscard]] BIO::Result getBioResult() {
            BIO::Result result;
            if ( _state == State::closed ) {
                _bio.closed();
            } else {
                result = _bio.getResult();
                _pendingDownstreamWrites += result.toWrite.size;
                if ( _state == State::closing && _pendingDownstreamWrites == 0 ) _factory.close(this);
            }
            return result;
        }

        /// Sends the results of BIO calls to the _factory.
        /// My _mutex must be unlocked!
        void performBIO(BIO::Result& bioResult) {
            logDebug("performBIO: completed %zu bytes, writing %zu", bioResult.bytesRead, bioResult.toWrite.size);
            if ( bioResult.bytesRead > 0 ) _factory.completedReceive(this, bioResult.bytesRead);
            if ( bioResult.toWrite.size > 0 ) _factory.write(this, C4SliceResult(std::move(bioResult.toWrite)));
        }

        // -------- error handling


        // Checks an mbedTLS read/write return value for errors. Returns false on error, true if ok.
        ssize_t check(int mbedResult) {
            if ( mbedResult >= 0 ) [[likely]]
                return true;
            C4Error error;
            switch ( mbedResult ) {
                case MBEDTLS_ERR_SSL_WANT_READ:
                case MBEDTLS_ERR_SSL_WANT_WRITE:
                case MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS:
                    return true;
                case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
                case MBEDTLS_ERR_NET_CONN_RESET:
                    error = C4Error::make(POSIXDomain, ECONNRESET);
                    break;
                case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED:
                    error = C4Error::make(NetworkDomain, kC4NetErrTLSCertUntrusted);
                    break;
                case MBEDTLS_ERR_NET_RECV_FAILED:  // Are these even possible?
                case MBEDTLS_ERR_NET_SEND_FAILED:
                    error = C4Error::make(POSIXDomain, EIO);
                    break;
                default:
                    error = C4Error::make(MbedTLSDomain, mbedResult);
                    break;
            }
            setError(error);
            return false;
        }

        void setError(C4Error const& error) {
            logError("Error: %s", error.description().c_str());
            assert_precondition(error);
            if ( !_error ) {
                _error = error;
                if ( _state < State::closing ) _state = State::closing;
            }
        }


        enum class State { closed, handshake, open, closing };


        static constexpr size_t kBufferSize = 16384;

        recursive_mutex                 _mutex;
        Ref<TLSContext>                 _tlsContext;
        string                          _url;
        State                           _state = State::closed;
        unique_ptr<mbedtls_ssl_context> _ssl;
        BIO                             _bio;
        RingBuffer                      _cleartextSendBuffer;
        std::unique_ptr<uint8_t[]>      _cleartextRecvBuffer;
        size_t                          _pendingDownstreamWrites = 0;
        size_t                          _pendingUpstreamReceived = 0;
        C4Error                         _error{};
    };

    pair<C4SocketFactory, void*> wrapSocketInTLS(const C4SocketFactory& factory, void* nativeHandle,
                                                 TLSContext* tlsContext) {
        auto socket = retain(new TLSSocket(factory, nativeHandle, tlsContext));
        return {socket->factory(), nativeHandle ? socket : nullptr};
    }

}  // namespace litecore::net
