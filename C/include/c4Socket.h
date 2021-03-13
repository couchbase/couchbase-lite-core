//
// c4Socket.h
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

#pragma once
#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Socket  Replication Socket Provider API
        @{ */
    
    /** Standard WebSocket close status codes, for use in C4Errors with WebSocketDomain.
        These are defined at <http://tools.ietf.org/html/rfc6455#section-7.4.1> */
    typedef C4_ENUM(int32_t, C4WebSocketCloseCode) {
        kWebSocketCloseNormal           = 1000,
        kWebSocketCloseGoingAway        = 1001, // Peer has to close, e.g. because host app is quitting
        kWebSocketCloseProtocolError    = 1002, // Protocol violation: invalid framing data
        kWebSocketCloseDataError        = 1003, // Message payload cannot be handled
        kWebSocketCloseNoCode           = 1005, // No status code in close frame
        kWebSocketCloseAbnormal         = 1006, // Peer closed socket unexpectedly w/o a close frame
        kWebSocketCloseBadMessageFormat = 1007, // Unparseable message
        kWebSocketClosePolicyError      = 1008,
        kWebSocketCloseMessageTooBig    = 1009,
        kWebSocketCloseMissingExtension = 1010, // Peer doesn't provide a necessary extension
        kWebSocketCloseCantFulfill      = 1011, // Can't fulfill request due to "unexpected condition"
        kWebSocketCloseTLSFailure       = 1015, // Never sent, only received

        kWebSocketCloseAppTransient     = 4001, // App-defined transient error
        kWebSocketCloseAppPermanent     = 4002, // App-defined permanent error
        
        kWebSocketCloseFirstAvailable   = 5000, // First unregistered code for freeform use
    };


    /** Represents an open bidirectional stream of bytes or messages (typically a TCP socket.)
        C4Socket is allocated and freed by LiteCore, but the client can associate it with a native
        stream/socket (like a file descriptor or a Java stream reference) by storing a value in its
        `nativeHandle` field. */
    struct C4Socket {
        void* C4NULLABLE nativeHandle;     ///< for client's use
    };


    /** The type of message framing that should be applied to the socket's data (added to outgoing,
        parsed out of incoming.) */
    typedef C4_ENUM(uint8_t, C4SocketFraming) {
        kC4WebSocketClientFraming,  ///< Frame as WebSocket client messages (masked)
        kC4NoFraming,               ///< No framing; use messages as-is
        kC4WebSocketServerFraming,  ///< Frame as WebSocket server messages (not masked)
    };


    /** A group of callbacks that define the implementation of sockets; the client must fill this
        out and pass it to c4socket_registerFactory() before using any socket-based API.
        These callbacks will be invoked on arbitrary background threads owned by LiteCore.
        They should return quickly, and perform the operation asynchronously without blocking. */
    struct C4SocketFactory {
        /** This should be set to `kC4NoFraming` if the socket factory acts as a stream of messages,
            `kC4WebSocketClientFraming` or `kC4WebSocketServerFraming` if it's a byte stream. */
        C4SocketFraming framing;

        /** An arbitrary value that will be passed to the `open` callback. */
        void* C4NULLABLE context;

        /** Called to open a socket to a destination address, asynchronously.
            @param socket  A new C4Socket instance to be opened. Its `nativeHandle` will be NULL;
                           the implementation of this function will probably store a native socket
                           reference there. This function should return immediately instead of
                           waiting for the connection to open. */
        void (*open)(C4Socket* socket,
                     const C4Address* addr,
                     C4Slice options,
                     void* C4NULLABLEcontext);

        /** Called to write to the socket. If `framing` equals to `kC4NoFraming`, the data is a complete
            message, and the socket implementation is responsible for framing it;
            in other case, it's just raw bytes to write to the stream, including the necessary framing.
            @param socket  The socket to write to.
            @param allocatedData  The data/message to send. As this is a `C4SliceResult`, the
                implementation of this function is responsible for freeing it when done. */
        void (*write)(C4Socket* socket, C4SliceResult allocatedData);

        /** Called to inform the socket that LiteCore has finished processing the data from a
            `c4socket_received()` call. This can be used for flow control.
            @param socket  The socket that whose incoming data was processed.
            @param byteCount  The number of bytes of data processed (the size of the `data`
                slice passed to a `c4socket_received()` call.) */
        void (*completedReceive)(C4Socket* socket, size_t byteCount);

        /** Called to close the socket.  This is only called if `framing` doesn't equal `kC4NoFraming`, i.e.
            the socket operates at the byte level. Otherwise it may be left NULL.
            No more write calls will be made; the socket should process any remaining incoming bytes
            by calling `c4socket_received()`, then call `c4socket_closed()` when the socket closes.
            @param socket  The socket to close. */
        void (* C4NULLABLE close)(C4Socket* socket);

        /** Called to close the socket.  This is only called if `framing` equals to `kC4NoFraming`, i.e.
            the socket operates at the message level.  Otherwise it may be left NULL.
            The implementation should send a message that tells the remote peer that it's closing
            the connection, then wait for acknowledgement before closing.
            No more write calls will be made; the socket should process any remaining incoming
            messages by calling `c4socket_received()`, then call `c4socket_closed()` when the
            connection closes.
            @param socket  The socket to close. */
        void (* C4NULLABLE requestClose)(C4Socket* socket, int status, C4String message);

        /** Called to tell the client that a `C4Socket` object is being disposed/freed after it's
            closed. The implementation of this function can then dispose any state associated with
            the `nativeHandle`.
            Set this to NULL if you don't need the call.
            @param socket  The socket being disposed.  */
        void (* C4NULLABLE dispose)(C4Socket* socket);
    };


    /** One-time registration of socket callbacks. Must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
    void c4socket_registerFactory(C4SocketFactory factory) C4API;

    /** Notification that a socket has received an HTTP response, with the given headers (encoded
        as a Fleece dictionary.) This should be called just before c4socket_opened or
        c4socket_closed.
        @param socket  The socket being opened.
        @param httpStatus  The HTTP/WebSocket status code from the peer; expected to be 200 if the
            connection is successful, else an HTTP status >= 300 or WebSocket status >= 1000.
        @param responseHeadersFleece  The HTTP response headers, encoded as a Fleece dictionary
            whose keys are the header names (with normalized case) and values are header values
            as strings. */
    void c4socket_gotHTTPResponse(C4Socket *socket,
                                  int httpStatus,
                                  C4Slice responseHeadersFleece) C4API;

    /** Notifies LiteCore that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully.
        @param socket  The socket. */
    void c4socket_opened(C4Socket *socket) C4API;

    /** Notifies LiteCore that a socket has finished closing, or disconnected, or failed to open.
        - If this is a normal close in response to a C4SocketFactory.close request, the error
          parameter should have a code of 0.
        - If it's a socket-level error, set the C4Error appropriately.
        - If it's a WebSocket-level close (when the factory's `framing` equals to `kC4NoFraming`),
          set the error domain to WebSocketDomain and the code to the WebSocket status code.
        @param socket  The socket.
        @param errorIfAny  the status of the close; see description above. */
    void c4socket_closed(C4Socket *socket, C4Error errorIfAny) C4API;

    /** Notifies LiteCore that the peer has requested to close the socket using the WebSocket protocol.
        (Should only be called by sockets whose factory's `framing` equals to `kC4NoFraming`.)
        LiteCore will call the factory's requestClose callback in response when it's ready to
        acknowledge the close.
        @param socket  The socket.
        @param  status  The WebSocket status sent by the peer, typically 1000.
        @param  message  An optional human-readable message sent by the peer. */
    void c4socket_closeRequested(C4Socket *socket, int status, C4String message);

    /** Notifies LiteCore that a C4SocketFactory.write request has been completed, i.e. the bytes
        have been written to the socket.
        @param socket  The socket.
        @param byteCount  The number of bytes that were written. */
    void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API;

    /** Notifies LiteCore that data was received from the socket. If the factory's
        `framing` equals to `kC4NoFraming`, the data must be a single complete message; otherwise it's
        raw bytes that will be un-framed by LiteCore.
        LiteCore will acknowledge when it's received and processed the data, by calling
        C4SocketFactory.completedReceive. For flow-control purposes, the client should keep track
        of the number of unacknowledged bytes, and stop reading from the underlying stream if that
        grows too large.
        @param socket  The socket.
        @param data  The data received, either a message or raw bytes. */
    void c4socket_received(C4Socket *socket, C4Slice data) C4API;


    /** Constructs a C4Socket from a "native handle", whose interpretation is up to the
        C4SocketFactory.  This is used by listeners to handle an incoming replication connection.
        @param factory  The C4SocketFactory that will manage the socket.
        @param nativeHandle  A value known to the factory that represents the underlying socket,
            such as a file descriptor or a native object pointer.
        @param address  The address of the remote peer making the connection.
        @return  A new C4Socket initialized with the `nativeHandle`. */
    C4Socket* c4socket_fromNative(C4SocketFactory factory,
                                  void *nativeHandle,
                                  const C4Address *address) C4API;


    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
