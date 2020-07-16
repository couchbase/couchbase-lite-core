//
// TLSContext.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include <functional>
#include <memory>

namespace sockpp {
    class mbedtls_context;
}
namespace litecore {
    class LogDomain;
}
namespace litecore::crypto {
    class Cert;
    struct Identity;
}

namespace litecore { namespace net {

    /** TLS configuration for sockets and listeners.
        A thin veneer around sockpp::tls_context.

        This class provides four methods of TLS certificate verification:
     
        1. Use the system trust store, and fail if there is a problem with the certificate chain (default)
        2. Provide your own chain of trusted root certificates to use in place of the system trust store.
        3. Only allow self-signed certificates (that are otherwise valid, other than being untrusted).  This mode is useful for ad-hoc P2P networks.
        4. Use a callback to examine TLS certificates that have failed verification
     
        These modes cannot be combined.
     */
    class TLSContext : public fleece::RefCounted {
    public:
        enum role_t {
            Client,
            Server
        };

        explicit TLSContext(role_t);

        // Use the specified root certificates as a trust store, ignoring the system
        // provided one.  This will override any previous calls to allowOnlySelfSigned
        // or setCertAuthCallback.
        void setRootCerts(crypto::Cert* NONNULL);
        
        // Passing nullslice here resets the behavior to using the system trust store
        void setRootCerts(fleece::slice);

        void requirePeerCert(bool);

        // Trust this certificate ultimately, and nothing else.  Calling this will
        // override the other three trust modes (allowOnlySelfSigned, setRootCerts,
        // and setCertAuthCallback)
        void allowOnlyCert(crypto::Cert* NONNULL);
        
        // Passing nullslice here resets the behavior to using the system trust store
        void allowOnlyCert(fleece::slice certData);
        
        // Used for P2P where remote certs are often dynamically generated
        // This will override any previous calls to setCertAuthCallback
        // or setRootCerts.  Passing false will reset the behavior to using
        // the system trust store.
        void allowOnlySelfSigned(bool);
        bool onlySelfSignedAllowed() const { return _onlySelfSigned; }

        // Use a callback to evaluate a TLS certificate that was otherwise deemed
        // unusable.  As a side effect, this function restores the system trust
        // store.
        void setCertAuthCallback(std::function<bool(fleece::slice)>);

        void setIdentity(crypto::Identity* NONNULL);
        void setIdentity(fleece::slice certData, fleece::slice privateKeyData);

    protected:
        ~TLSContext();
        bool findSigningRootCert(const std::string &certStr, std::string &rootStr);

    private:
        void resetRootCertFinder();
        
        std::unique_ptr<sockpp::mbedtls_context> _context;
        fleece::Retained<crypto::Identity> _identity;
        role_t _role;
        bool _onlySelfSigned {false};

        friend class TCPSocket;
    };

} }
