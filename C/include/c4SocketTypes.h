//
// c4SocketTypes.h
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Socket  Replication Socket Provider API
    @{ */

/** Standard WebSocket close status codes, for use in C4Errors with WebSocketDomain.
    These are defined at <http://tools.ietf.org/html/rfc6455#section-7.4.1> */
typedef C4_ENUM(int32_t, C4WebSocketCloseCode){
        kWebSocketCloseNormal           = 1000,
        kWebSocketCloseGoingAway        = 1001,  // Peer has to close, e.g. because host app is quitting
        kWebSocketCloseProtocolError    = 1002,  // Protocol violation: invalid framing data
        kWebSocketCloseDataError        = 1003,  // Message payload cannot be handled
        kWebSocketCloseNoCode           = 1005,  // No status code in close frame
        kWebSocketCloseAbnormal         = 1006,  // Peer closed socket unexpectedly w/o a close frame
        kWebSocketCloseBadMessageFormat = 1007,  // Unparseable message
        kWebSocketClosePolicyError = 1008,      kWebSocketCloseMessageTooBig = 1009,
        kWebSocketCloseMissingExtension = 1010,  // Peer doesn't provide a necessary extension
        kWebSocketCloseCantFulfill      = 1011,  // Can't fulfill request due to "unexpected condition"
        kWebSocketCloseTLSFailure       = 1015,  // Never sent, only received

        kWebSocketCloseAppTransient = 4001,  // App-defined transient error
        kWebSocketCloseAppPermanent = 4002,  // App-defined permanent error

        kWebSocketCloseFirstAvailable = 5000,  // First unregistered code for freeform use
};


/** The type of message framing that should be applied to the socket's data (added to outgoing,
    parsed out of incoming.) */
typedef C4_ENUM(uint8_t, C4SocketFraming){
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

    /** Called to open a socket to a destination address.
        This function should operate asynchronously, returning immediately.
        When the socket opens, call \ref c4socket_opened, or if it fails to open, call
        \ref c4socket_closed with an appropriate error.

        @param socket  A new C4Socket instance to be opened. Its `nativeHandle` will be NULL;
                       the implementation of this function will probably store a native socket
                       reference there.
        @param addr  The address (URL) to connect to.
        @param options  A Fleece-encoded dictionary containing additional parameters, such as
                        `kC4SocketOptionWSProtocols`, which provides the WebSocket protocol names
                        to include in the HTTP request header.
        @param context  The value of the `C4SocketFactory`'s `context` field. */
    void (*open)(C4Socket* socket, const C4Address* addr, C4Slice options, void* C4NULLABLE context);

    /** Called to write to the socket. If `framing` equals `kC4NoFraming`, the data is a complete
        message, and your socket implementation is responsible for framing it;
        otherwise, it's just raw bytes to write to the stream, including the necessary WebSocket
        framing.

        After data has been written, you must call \ref c4socket_completedWrite. (You can call this
        once after all the data is written, or multiple times with smaller numbers, as long as the
        total byte count equals the size of the `allocatedData`.)

        @param socket  The socket to write to.
        @param allocatedData  The data/message to send. As this is a `C4SliceResult`, your
            implementation of this function is responsible for releasing it when done. */
    void (*write)(C4Socket* socket, C4SliceResult allocatedData);

    /** Called to inform the socket that LiteCore has finished processing the data from a
        \ref c4socket_received call.

        This can be used for flow control: you should keep track of
        the number of bytes you've sent to LiteCore, incrementing the number when you call
        \ref c4socket_received, and decrementing it when `completedReceived` is called.
        If the number goes above some threshold, you should take measures to stop receiving more
        data, e.g. by not reading more bytes from the socket. Otherwise memory usage can balloon as
        more and more data arrives and must be buffered in memory.

        @param socket  The socket that whose incoming data was processed.
        @param byteCount  The number of bytes of data processed (the size of the `data`
            slice passed to a `c4socket_received()` call.) */
    void (*completedReceive)(C4Socket* socket, size_t byteCount);

    /** Called to close the socket.  This is only called if `framing` doesn't equal `kC4NoFraming`, i.e.
        the socket operates at the byte level. Otherwise it may be left NULL.
        No more write calls will be made; you should process any remaining incoming bytes
        by calling \ref c4socket_received, then call \ref c4socket_closed when the socket closes.
        \warning You MUST call \ref c4socket_closed, or the replicator will wait forever!

        @param socket  The socket to close. */
    void (*C4NULLABLE close)(C4Socket* socket);

    /** Called to close the socket.  This is only called if `framing` equals to `kC4NoFraming`, i.e.
        the socket operates at the message level.  Otherwise it may be left NULL.

        Your implementation should:
        1. send a message that tells the remote peer that it's closing the connection;
        2. wait for acknowledgement;
        3. while waiting, handle any further incoming messages by calling \ref c4socket_received;
        4. after 5 seconds of waiting, give up and go to step 5;
        5. upon acknowledgement or timeout, close its connection and call \ref c4socket_closed.

        This call can also occur _before_ the socket has opened, if the replicator decides to time
        out. In this situation -- i.e. if you have not yet called \ref c4socket_opened --
        you should tear down your connection and call \ref c4socket_closed.

        \warning You MUST call \ref c4socket_closed, or the replicator will wait forever!

        @param socket  The `C4Socket` to close.
        @param status  The WebSocket status code to send in the CLOSE message.
        @param message  The text to send in the CLOSE message. */
    void (*C4NULLABLE requestClose)(C4Socket* socket, int status, C4String message);

    /** Called to tell the client that a `C4Socket` object is being disposed/freed after it's
        closed. The implementation of this function can then dispose any state associated with
        the `nativeHandle`.

        Set this to NULL if you don't need the call.

        @param socket  The socket being disposed.  */
    void (*C4NULLABLE dispose)(C4Socket* socket);
};

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
