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
#include "c4ListenerTypes.h"
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
struct C4Socket
    : public fleece::InstanceCounted
    , C4Base {
    /** One-time registration of socket callbacks. Must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
    static void registerFactory(const C4SocketFactory& factory);

    static bool                   hasRegisteredFactory();
    static const C4SocketFactory& registeredFactory();

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

    /** Returns true if the C4Socket wants to do its own certificate validation.
     *  If so, the factory should disable all of its own certificate validation. */
    [[nodiscard]] virtual bool hasCustomPeerCertValidation() const { return false; }

    /** Notification that a socket is making a TLS connection and has received the peer's (usually
        server's) certificate.
        This notification occurs only after any other TLS validation options have passed
        (`kC4ReplicatorOptionRootCerts`, `kC4ReplicatorOptionPinnedServerCert`,
        `kC4ReplicatorOptionOnlySelfSignedServerCert`).
        This notification occurs before \ref gotHTTPResponse() or \ref opened().
        @param certData  The DER-encoded form of the peer's TLS certificate.
        @param hostname  The DNS hostname of the peer. (This may be different from the original
                         Address given, if there were HTTP redirects.)
        @returns  True to proceed, false to abort the connection. */
    [[nodiscard]] virtual bool gotPeerCertificate(slice certData, std::string_view hostname) = 0;

    /** Notification that a client socket has received an HTTP response, with the given headers (encoded
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
    void setNativeHandle(void* C4NULLABLE h) { _nativeHandle = h; }

    C4SocketFactory const& getFactory() const { return _factory; }

    /** Returns the opaque "native handle" (e.g. a Unix file descriptor or C `FILE*`) that you've
        associated with the socket. */
    void* C4NULLABLE getNativeHandle() { return _nativeHandle; }

  protected:
    friend C4Socket* C4NULLABLE c4socket_retain(C4Socket* C4NULLABLE socket) C4API;
    friend void                 c4socket_release(C4Socket* C4NULLABLE socket) C4API;

    C4Socket(C4SocketFactory const&, void* C4NULLABLE nativeHandle = nullptr);
    ~C4Socket() override;

    // Retain/release have to be abstracted bc C4Socket does not itself inherit from RefCounted.
    virtual void socket_retain()  = 0;
    virtual void socket_release() = 0;

    C4SocketFactory const _factory;
    void* C4NULLABLE      _nativeHandle;  ///< for client's use
};

// Glue to make Retained<C4Socket> work:
inline C4Socket* retain(C4Socket* C4NULLABLE socket) { return c4socket_retain(socket); }

inline void release(C4Socket* C4NULLABLE socket) { c4socket_release(socket); }

/** Abstract implementation of a socket factory, wrapping C4SocketFactory in a C++ API.
 *  A convenience for protocol implementors. */
class C4SocketFactoryImpl
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4SocketFactoryImpl>
    , protected C4Base {
  public:
    /// The C4Socket I implement. Null until opened.
    C4Socket* socket() const FLPURE { return _socket; }

  protected:
    explicit C4SocketFactoryImpl(C4Socket* socket);
    C4SocketFactoryImpl() = default;

    void releaseSocket() { _socket = nullptr; }

    //-------- My C4SocketFactory "methods"; called by my socket

    /// Called by `C4SocketFactory::attached`. You probably don't need to override it.
    virtual void attached();

    /// Called by `C4SocketFactory::open`.
    virtual void open(const C4Address& address, C4Slice options) = 0;

    /// Called by `C4SocketFactory::write`.
    virtual void write(fleece::alloc_slice data) = 0;

    /// Called by `C4SocketFactory::completedReceive`.
    virtual void completedReceive(size_t byteCount) = 0;

    /// Called by `C4SocketFactory::close()`.
    virtual void close() = 0;

  private:
    template <std::derived_from<C4SocketFactoryImpl> FACTORY>
    friend C4SocketFactory c4SocketFactoryFor();

    static const C4SocketFactory kFactory;
    static C4SocketFactoryImpl*  nativeHandle(C4Socket*);

    fleece::Retained<C4Socket> _socket;
};

/// Returns a C4SocketFactory that can be used to open a C4Socket
/// using a specific C4SocketFactoryImpl subclass.
template <std::derived_from<C4SocketFactoryImpl> FACTORY>
C4SocketFactory c4SocketFactoryFor() {
    auto fac = C4SocketFactoryImpl::kFactory;
    fac.open = [](C4Socket* socket, const C4Address* address, C4Slice options, void* context) {
        C4SocketFactoryImpl* impl;
        if ( auto native = socket->getNativeHandle() ) impl = static_cast<C4SocketFactoryImpl*>(native);
        else
            impl = new FACTORY(socket);
        impl->open(*address, options);
    };
    return fac;
};

/** @} */

C4_ASSUME_NONNULL_END
