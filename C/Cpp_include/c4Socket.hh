//
// c4Socket.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4SocketTypes.h"  // IWYU pragma: keep - We need full definitions for the types
#include "fleece/InstanceCounted.hh"

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** \defgroup Socket  Replication Socket Provider API
    @{ */


/** Represents an open bidirectional stream of bytes or messages (typically a TCP socket.)
    C4Socket is allocated and freed by LiteCore, but the client can associate it with a native
    stream/socket (like a file descriptor or a Java stream reference) by storing a value in its
    `nativeHandle` field. */
struct C4Socket  // NOLINT(cppcoreguidelines-pro-type-member-init) - its okay for nativeHandle to be null
    : public fleece::InstanceCounted
    , C4Base {
    /** One-time registration of socket callbacks. Must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
    static void registerFactory(const C4SocketFactory& factory);

    /** Constructs a C4Socket from a "native handle", whose interpretation is up to the
        C4SocketFactory.  This is used by listeners to handle an incoming replication connection.
        @warning  You MUST immediately call `c4socket_retain` on this pointer (and the usual
                  `c4socket_release` when done.) This is inconsistent with the general ref-counting
                  convention, but fixing this function to return a retained value would cause all
                  existing platforms to leak C4Sockets, so we're leaving it alone.
        @param factory  The C4SocketFactory that will manage the socket.
        @param nativeHandle  A value known to the factory that represents the underlying socket,
            such as a file descriptor or a native object pointer.
        @param address  The address of the remote peer.
        @param incoming  True if this is an incoming (server) connection, false for outgoing (client).
        @return  A new C4Socket initialized with the `nativeHandle`. */
    static C4Socket* fromNative(const C4SocketFactory& factory, void* C4NULLABLE nativeHandle, const C4Address& address,
                                bool incoming = true);


    /** Notification that a socket has received an HTTP response, with the given headers (encoded
        as a Fleece dictionary.) This should be called just before \ref opened() or \ref closed().
        @param httpStatus  The HTTP/WebSocket status code from the peer; expected to be 200 if the
            connection is successful, else an HTTP status >= 300 or WebSocket status >= 1000.
        @param responseHeadersFleece  The HTTP response headers, encoded as a Fleece dictionary
            whose keys are the header names (with normalized case) and values are header values
            as strings. */
    virtual void gotHTTPResponse(int httpStatus, slice responseHeadersFleece) = 0;

    /** Notifies LiteCore that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully. */
    virtual void opened() = 0;

    /** Notifies LiteCore that a socket has finished closing, or disconnected, or failed to open.
        - If this is a normal close in response to a C4SocketFactory.close request, the error
          parameter should have a code of 0.
        - If it's a socket-level error, set the C4Error appropriately.
        - If it's a WebSocket-level close (when the factory's `framing` equals to `kC4NoFraming`),
          set the error domain to WebSocketDomain and the code to the WebSocket status code.
        @param errorIfAny  the status of the close; see description above. */
    virtual void closed(C4Error errorIfAny) = 0;

    /** Notifies LiteCore that the peer has requested to close the socket using the WebSocket protocol.
        (Should only be called by sockets whose factory's `framing` equals to `kC4NoFraming`.)
        LiteCore will call the factory's requestClose callback in response when it's ready to
        acknowledge the close.
        @param  status  The WebSocket status sent by the peer, typically 1000.
        @param  message  An optional human-readable message sent by the peer. */
    virtual void closeRequested(int status, slice message) = 0;

    /** Notifies LiteCore that a C4SocketFactory.write request has been completed, i.e. the bytes
        have been written to the socket.
        @param byteCount  The number of bytes that were written. */
    virtual void completedWrite(size_t byteCount) = 0;

    /** Notifies LiteCore that data was received from the socket. If the factory's
        `framing` equals to `kC4NoFraming`, the data must be a single complete message; otherwise it's
        raw bytes that will be un-framed by LiteCore.
        LiteCore will acknowledge when it's received and processed the data, by calling
        C4SocketFactory.completedReceive. For flow-control purposes, the client should keep track
        of the number of unacknowledged bytes, and stop reading from the underlying stream if that
        grows too large.
        @param data  The data received, either a message or raw bytes. */
    virtual void received(slice data) = 0;

    /** Stores an opaque value to associate with this object,
        e.g. a Unix file descriptor or C `FILE*`. */
    void setNativeHandle(void* C4NULLABLE h) { nativeHandle = h; }

    /** Returns the opaque "native handle" (e.g. a Unix file descriptor or C `FILE*`) that you've
        associated with the socket. */
    void* C4NULLABLE getNativeHandle() { return nativeHandle; }

  protected:
    C4Socket() = default;
    ~C4Socket() override;

    void* C4NULLABLE nativeHandle;  ///< for client's use
};

/** @} */

C4_ASSUME_NONNULL_END
