//
// c4Socket.h
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
#include "c4SocketTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Socket  Replication Socket Provider API
        @{ */


// NOTE: C4Socket used to be a concrete struct containing a single field `nativeHandle`.
// As part of creating the C++ API, this struct declaration was removed so it could be
// declared in c4Struct.hh as a real C++ object.
// To fix client code that accessed `nativeHandle` directly, call `c4Socket_setNativeHandle`
// and/or `c4Socket_setNativeHandle` instead.


/** One-time registration of default socket callbacks. If used, must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
CBL_CORE_API void c4socket_registerFactory(C4SocketFactory factory) C4API;

/** Associates an opaque "native handle" with this object. You can use this to store whatever
        you need to represent the socket's implementation, like a file descriptor. 
        \note The caller must use a lock for Socket when this function is called. */
CBL_CORE_API void c4Socket_setNativeHandle(C4Socket*, void* C4NULLABLE) C4API;

/** Returns the opaque "native handle" associated with this object.
        \note The caller must use a lock for Socket when this function is called. */
CBL_CORE_API void* C4NULLABLE c4Socket_getNativeHandle(C4Socket*) C4API;

/** Notifies LiteCore that a socket is making a TLS connection and has received the peer's (usually
    server's) certificate, so that it knows the cert and can call any custom auth callbacks.
    This function MUST be called if there is a valid peer cert.

    You should first perform other TLS validation, both platform-specific and as specified by
    the options `kC4ReplicatorOptionRootCerts`, `kC4ReplicatorOptionPinnedServerCert`,
    `kC4ReplicatorOptionOnlySelfSignedServerCert`. If any of those fail, close the socket.
    (But if `kC4ReplicatorOptionAcceptAllCerts` is set, none of the above checks are done.)

    After other validation succeeds, call this function -- before \ref c4socket_gotHTTPResponse() or
    \ref c4socket_opened(). If it returns true, proceed. If it returns false, the certificate is rejected
    and you should close the socket immediately with error `kC4NetErrTLSCertUntrusted`.

    \note The caller must use a lock for Socket when this function is called.
    @param socket  The socket being opened.
    @param certData  The DER-encoded data of the peer's TLS certificate.
    @param hostname  The DNS hostname of the peer. (This may be different from the original
                     Address given, if there were HTTP redirects.)
    @returns  True to proceed, false to abort the connection. */
CBL_CORE_API bool c4socket_gotPeerCertificate(C4Socket* socket, C4Slice certData, C4String hostname) C4API;

/** Notification that a client socket has received an HTTP response, with the headers encoded
    as a Fleece dictionary.
    This call is required for a client socket (where the socket factory's `open` function was called.)
    It should not be called on a server/incoming socket (where \ref c4socket_fromNative was called.)

    This should be called just before \ref c4socket_opened or \ref c4socket_closed.
    \note The caller must use a lock for Socket when this function is called.
    @param socket  The socket being opened.
    @param httpStatus  The HTTP/WebSocket status code from the peer; expected to be 200 if the
        connection is successful, else an HTTP status >= 300 or WebSocket status >= 1000.
    @param responseHeadersFleece  The HTTP response headers, encoded as a Fleece dictionary
        whose keys are the header names (with normalized case) and values are header values
        as strings. */
CBL_CORE_API void c4socket_gotHTTPResponse(C4Socket* socket, int httpStatus, C4Slice responseHeadersFleece) C4API;

/** Notifies LiteCore that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully.
        \note The caller must use a lock for Socket when this function is called.
        @param socket  The socket. */
CBL_CORE_API void c4socket_opened(C4Socket* socket) C4API;

/** Notifies LiteCore that a socket has finished closing, or disconnected, or failed to open.
        - If this is a normal close in response to a C4SocketFactory.close request, the error
          parameter should have a code of 0.
        - If it's a socket-level error, set the C4Error appropriately.
        - If it's a WebSocket-level close (when the factory's `framing` equals to `kC4NoFraming`),
          set the error domain to WebSocketDomain and the code to the WebSocket status code.
        \note The caller must use a lock for Socket when this function is called.
        @param socket  The socket.
        @param errorIfAny  the status of the close; see description above. */
CBL_CORE_API void c4socket_closed(C4Socket* socket, C4Error errorIfAny) C4API;

/** Notifies LiteCore that the peer has requested to close the socket using the WebSocket protocol.
        (Should only be called by sockets whose factory's `framing` equals to `kC4NoFraming`.)
        LiteCore will call the factory's requestClose callback in response when it's ready to
        acknowledge the close.
        \note The caller must use a lock for Socket when this function is called.
        @param socket  The socket.
        @param  status  The WebSocket status sent by the peer, typically 1000.
        @param  message  An optional human-readable message sent by the peer. */
CBL_CORE_API void c4socket_closeRequested(C4Socket* socket, int status, C4String message) C4API;

/** Notifies LiteCore that a C4SocketFactory.write request has been completed, i.e. the bytes
        have been written to the socket.
        \note The caller must use a lock for Socket when this function is called.
        @param socket  The socket.
        @param byteCount  The number of bytes that were written. */
CBL_CORE_API void c4socket_completedWrite(C4Socket* socket, size_t byteCount) C4API;

/** Notifies LiteCore that data was received from the socket. If the factory's
        `framing` equals to `kC4NoFraming`, the data must be a single complete message; otherwise it's
        raw bytes that will be un-framed by LiteCore.
        LiteCore will acknowledge when it's received and processed the data, by calling
        C4SocketFactory.completedReceive. For flow-control purposes, the client should keep track
        of the number of unacknowledged bytes, and stop reading from the underlying stream if that
        grows too large.
        \note The caller must use a lock for Socket when this function is called.
        @param socket  The socket.
        @param data  The data received, either a message or raw bytes. */
CBL_CORE_API void c4socket_received(C4Socket* socket, C4Slice data) C4API;


NODISCARD CBL_CORE_API C4Socket* c4socket_fromNative(C4SocketFactory factory, void* nativeHandle,
                                                     const C4Address* address) C4API;

/** Constructs a C4Socket from a "native handle", whose interpretation is up to the C4SocketFactory.
        \note This function is thread-safe.
        @note Unlike `c4socket_fromNative`, this returns a retained C4Socket you are responsible for releasing.

        @param factory  The C4SocketFactory that will manage the socket.
        @param nativeHandle  A value known to the factory that represents the underlying socket,
            such as a file descriptor or a native object pointer.
        @param address  The address of the remote peer.
        @param incoming  True if this is an incoming (server) connection, false if outgoing (client).
        @return  A new C4Socket initialized with the `nativeHandle`. */
NODISCARD CBL_CORE_API C4Socket* c4socket_fromNative2(C4SocketFactory factory, void* nativeHandle,
                                                      const C4Address* address, bool incoming) C4API;

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
