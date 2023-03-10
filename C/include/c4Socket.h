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
        you need to represent the socket's implementation, like a file descriptor. */
CBL_CORE_API void c4Socket_setNativeHandle(C4Socket*, void* C4NULLABLE) C4API;

/** Returns the opaque "native handle" associated with this object. */
CBL_CORE_API void* C4NULLABLE c4Socket_getNativeHandle(C4Socket*) C4API;

/** Notification that a socket has received an HTTP response, with the given headers (encoded
        as a Fleece dictionary.) This should be called just before c4socket_opened or
        c4socket_closed.
        @param socket  The socket being opened.
        @param httpStatus  The HTTP/WebSocket status code from the peer; expected to be 200 if the
            connection is successful, else an HTTP status >= 300 or WebSocket status >= 1000.
        @param responseHeadersFleece  The HTTP response headers, encoded as a Fleece dictionary
            whose keys are the header names (with normalized case) and values are header values
            as strings. */
CBL_CORE_API void c4socket_gotHTTPResponse(C4Socket* socket, int httpStatus, C4Slice responseHeadersFleece) C4API;

/** Notifies LiteCore that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully.
        @param socket  The socket. */
CBL_CORE_API void c4socket_opened(C4Socket* socket) C4API;

/** Notifies LiteCore that a socket has finished closing, or disconnected, or failed to open.
        - If this is a normal close in response to a C4SocketFactory.close request, the error
          parameter should have a code of 0.
        - If it's a socket-level error, set the C4Error appropriately.
        - If it's a WebSocket-level close (when the factory's `framing` equals to `kC4NoFraming`),
          set the error domain to WebSocketDomain and the code to the WebSocket status code.
        @param socket  The socket.
        @param errorIfAny  the status of the close; see description above. */
CBL_CORE_API void c4socket_closed(C4Socket* socket, C4Error errorIfAny) C4API;

/** Notifies LiteCore that the peer has requested to close the socket using the WebSocket protocol.
        (Should only be called by sockets whose factory's `framing` equals to `kC4NoFraming`.)
        LiteCore will call the factory's requestClose callback in response when it's ready to
        acknowledge the close.
        @param socket  The socket.
        @param  status  The WebSocket status sent by the peer, typically 1000.
        @param  message  An optional human-readable message sent by the peer. */
CBL_CORE_API void c4socket_closeRequested(C4Socket* socket, int status, C4String message) C4API;

/** Notifies LiteCore that a C4SocketFactory.write request has been completed, i.e. the bytes
        have been written to the socket.
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
        @param socket  The socket.
        @param data  The data received, either a message or raw bytes. */
CBL_CORE_API void c4socket_received(C4Socket* socket, C4Slice data) C4API;


/** Constructs a C4Socket from a "native handle", whose interpretation is up to the
        C4SocketFactory.  This is used by listeners to handle an incoming replication connection.
        @warning  You MUST immediately call `c4socket_retain` on this pointer (and the usual
                  `c4socket_release` when done.) This is inconsistent with the general ref-counting
                  convention, but fixing this function to return a retained value would cause all
                  existing platforms to leak C4Sockets, so we're leaving it alone.
        @param factory  The C4SocketFactory that will manage the socket.
        @param nativeHandle  A value known to the factory that represents the underlying socket,
            such as a file descriptor or a native object pointer.
        @param address  The address of the remote peer making the connection.
        @return  A new C4Socket initialized with the `nativeHandle`. */
CBL_CORE_API C4Socket* c4socket_fromNative(C4SocketFactory factory, void* nativeHandle, const C4Address* address) C4API;


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
