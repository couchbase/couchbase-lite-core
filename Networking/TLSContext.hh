//
// TLSContext.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include <functional>
#include <memory>

struct C4TLSConfig;

namespace sockpp {
    class mbedtls_context;
    class stream_socket;
    class tls_socket;
}  // namespace sockpp

namespace fleece {
    class Dict;
}

namespace litecore {
    class LogDomain;
}

namespace litecore::crypto {
    class Cert;
    struct Identity;
    class PrivateKey;
}  // namespace litecore::crypto

C4_ASSUME_NONNULL_BEGIN

namespace litecore::net {

    /** TLS configuration for sockets and listeners.
        A thin veneer around sockpp::tls_context.

        This class provides four methods of TLS certificate verification:
     
        1. Use the system trust store, and fail if there is a problem with the certificate chain (default)
        2. Provide your own chain of trusted root certificates to use in place of the system trust store.
        3. Only allow self-signed certificates (that are otherwise valid, other than being untrusted).
           This mode is useful for ad-hoc P2P networks.
        4. Use a callback to examine TLS certificates that have failed verification.
     
        These modes cannot be combined.
     */
    class TLSContext : public fleece::RefCounted {
      public:
        enum role_t { Client, Server };

        using CertAuthCallback = std::function<bool(fleece::slice)>;

        /// If the C4Replicator options in `options` (see C4ReplicatorTypes.h) require custom
        /// TLS settings, returns a properly configured client `TLSContext`.
        /// Else returns nullptr.
        /// @param options  Parsed replicator options dict.
        /// @param externalKey  Optional PrivateKey for the local identity.
        /// @param authCallback  Optional custom certificate auth callback.
        static fleece::Retained<TLSContext> fromReplicatorOptions(fleece::Dict                   options,
                                                                  crypto::PrivateKey* C4NULLABLE externalKey  = nullptr,
                                                                  const CertAuthCallback&        authCallback = {});

        /// Creates a server TLSContext based on the settings in the `C4TLSConfig`.
        /// @param config  The non-null C4TLSConfig from the C4ListenerConfig.
        /// @param c4Listener  The listener instance that will be passed as the first parameter
        ///                 to the config's `certAuthCallback`, if any.
        static fleece::Ref<TLSContext> fromListenerOptions(const C4TLSConfig* config, C4Listener* c4Listener);

        /// Creates a default TLSContext with either the client or server role.
        explicit TLSContext(role_t);

        /// Use the specified root certificates as a trust store, ignoring the system
        /// provided one.  This will override any previous calls to allowOnlySelfSigned
        /// or setCertAuthCallback.
        void setRootCerts(crypto::Cert*);

        /// Passing nullslice here resets the behavior to using the system trust store
        void setRootCerts(fleece::slice);

        /// Sets whether the peer is required to have a cert.
        void requirePeerCert(bool);

        /// Trust this certificate ultimately, and nothing else.  Calling this will
        /// override the other three trust modes (allowOnlySelfSigned, setRootCerts,
        /// and setCertAuthCallback)
        void allowOnlyCert(crypto::Cert*);

        /// Passing nullslice here resets the behavior to using the system trust store
        void allowOnlyCert(fleece::slice certData);

        /// True if `allowOnlyCert` has been called.
        bool onlyOneCertAllowed() const { return _onlyOneCert; }

        /// Used for P2P where remote certs are often dynamically generated
        /// This will override any previous calls to setCertAuthCallback
        /// or setRootCerts.  Passing false will reset the behavior to using
        /// the system trust store.
        void allowOnlySelfSigned(bool);

        bool onlySelfSignedAllowed() const { return _onlySelfSigned; }

        /// Use a callback to evaluate a TLS certificate that was otherwise deemed
        /// unusable.  As a side effect, this function restores the system trust
        /// store.
        void setCertAuthCallback(CertAuthCallback const&);

        /// Sets the local certificate and private key. Required for servers; optional for clients.
        void setIdentity(crypto::Identity*);
        void setIdentity(fleece::slice certData, fleece::slice privateKeyData);

        /// Performs the TLS handshake, then returns a wrapper socket that can be used for I/O.
        /// Be sure to check the returned socket's error status to see if the handshake failed.
        std::unique_ptr<sockpp::tls_socket> wrapSocket(std::unique_ptr<sockpp::stream_socket>,
                                                       const std::string& peer_name);

        sockpp::mbedtls_context& get_mbedtls_context() const FLPURE { return *_context; }

      protected:
        ~TLSContext() override;
        static bool findSigningRootCert(const std::string& certStr, std::string& rootStr);

      private:
        void resetRootCertFinder();

        std::unique_ptr<sockpp::mbedtls_context> _context;
        fleece::Retained<crypto::Identity>       _identity;
        role_t                                   _role;
        bool                                     _onlySelfSigned{false};
        bool                                     _onlyOneCert{false};
    };

}  // namespace litecore::net

C4_ASSUME_NONNULL_END
