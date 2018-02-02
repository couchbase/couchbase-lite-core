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


#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Socket  TCP Socket Provider API
        @{ */
    
    /** Standard WebSocket close status codes, for use in C4Errors with WebSocketDomain.
        These are defined at <http://tools.ietf.org/html/rfc6455#section-7.4.1> */
    typedef C4_ENUM(int32_t, C4WebSocketCloseCode) {
        kWebSocketCloseNormal           = 1000,
        kWebSocketCloseGoingAway        = 1001, // Peer has to close, e.g. because host app is quitting
        kWebSocketCloseProtocolError    = 1002, // Protocol violation: invalid framing data
        kWebSocketCloseDataError        = 1003, // Message payload cannot be handled
        kWebSocketCloseNoCode           = 1005, // Never sent, only received
        kWebSocketCloseAbnormal         = 1006, // Never sent, only received
        kWebSocketCloseBadMessageFormat = 1007, // Unparseable message
        kWebSocketClosePolicyError      = 1008,
        kWebSocketCloseMessageTooBig    = 1009,
        kWebSocketCloseMissingExtension = 1010, // Peer doesn't provide a necessary extension
        kWebSocketCloseCantFulfill      = 1011, // Can't fulfill request due to "unexpected condition"
        kWebSocketCloseTLSFailure       = 1015, // Never sent, only received

        kWebSocketCloseFirstAvailable   = 4000, // First unregistered code for freeform use
    };


    // Socket option dictionary keys:
    extern const char* const kC4SocketOptionWSProtocols; // Value for Sec-WebSocket-Protocol header
    
    
    /** A simple parsed-URL type */
    typedef struct {
        C4String scheme;
        C4String hostname;
        uint16_t port;
        C4String path;
    } C4Address;


    /** Represents an open bidirectional byte stream (typically a TCP socket.)
        C4Socket is allocated and freed by LiteCore, but the client can associate it with a native
        stream/socket (like a file descriptor or a Java stream reference) by storing a value in its
        `nativeHandle` field. */
    typedef struct C4Socket {
        void* nativeHandle;     ///< for client's use
    } C4Socket;


    /** A group of callbacks that define the implementation of sockets; the client must fill this
        out and pass it to c4socket_registerFactory() before using any socket-based API.
        These callbacks will be invoked on arbitrary background threads owned by LiteCore.
        They should return quickly, and perform the operation asynchronously without blocking.
     
        The `providesWebSockets` flag indicates whether this factory provides a WebSocket
        implementation or just a raw TCP socket. */
    typedef struct {
        bool providesWebSockets;

        void (*open)(C4Socket* C4NONNULL, const C4Address* C4NONNULL, C4Slice optionsFleece); ///< open the socket
        void (*write)(C4Socket* C4NONNULL, C4SliceResult allocatedData);  ///< Write bytes; free when done
        void (*completedReceive)(C4Socket* C4NONNULL, size_t byteCount);  ///< Completion of c4socket_received

        // Only called if providesWebSockets is false:
        void (*close)(C4Socket* C4NONNULL);                               ///< close the socket

        // Only called if providesWebSockets is true:
        void (*requestClose)(C4Socket* C4NONNULL, int status, C4String message);

        /** Called to tell the client to dispose any state associated with the `nativeHandle`.
            Set this to NULL if you don't need the call. */
        void (*dispose)(C4Socket* C4NONNULL);
} C4SocketFactory;


    /** One-time registration of socket callbacks. Must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
    void c4socket_registerFactory(C4SocketFactory factory) C4API;

    /** Notification that a socket has received an HTTP response, with the given headers (encoded
        as a Fleece dictionary.) This should be called just before c4socket_opened or
        c4socket_closed. */
    void c4socket_gotHTTPResponse(C4Socket *socket C4NONNULL,
                                  int httpStatus,
                                  C4Slice responseHeadersFleece) C4API;

    /** Notification that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully. */
    void c4socket_opened(C4Socket *socket C4NONNULL) C4API;

    /** Notification that a socket has finished closing, or that it disconnected, or failed to open.
        If this is a normal close in response to a C4SocketFactory.close request, the error
        parameter should have a code of 0.
        If it's a socket-level error, set the C4Error appropriately.
        If it's a WebSocket-level close (when the factory's providesWebSockets is true),
        set the error domain to WebSocketDomain and the code to the WebSocket status code. */
    void c4socket_closed(C4Socket *socket C4NONNULL, C4Error errorIfAny) C4API;

    /** Notification that the peer has requested to close the socket using the WebSocket protocol.
        LiteCore will call the factory's requestClose callback in response when it's ready. */
    void c4socket_closeRequested(C4Socket *socket C4NONNULL, int status, C4String message);

    /** Notification that bytes have been written to the socket, in response to a
        C4SocketFactory.write request. */
    void c4socket_completedWrite(C4Socket *socket C4NONNULL, size_t byteCount) C4API;

    /** Notification that bytes have been read from the socket. LiteCore will acknowledge receiving
        and processing the data by calling C4SocketFactory.completedReceive.
        For flow-control purposes, the client should keep track of the number of unacknowledged 
        bytes, and stop reading from the underlying stream if it grows too large. */
    void c4socket_received(C4Socket *socket C4NONNULL, C4Slice data) C4API;

    /** @} */

#ifdef __cplusplus
}
#endif
