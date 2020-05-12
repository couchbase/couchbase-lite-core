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
namespace litecore::crypto {
    class Cert;
    class Identity;
}

namespace litecore { namespace net {

    /** TLS configuration for sockets and listeners.
        A thin veneer around sockpp::tls_context. */
    class TLSContext : public fleece::RefCounted {
    public:
        enum role_t {
            Client,
            Server
        };

        explicit TLSContext(role_t);

        void setRootCerts(crypto::Cert* NONNULL);
        void setRootCerts(fleece::slice);

        void requirePeerCert(bool);

        void allowOnlyCert(crypto::Cert* NONNULL);
        void allowOnlyCert(fleece::slice certData);

        void setCertAuthCallback(std::function<bool(fleece::slice)>);

        void setIdentity(crypto::Identity* NONNULL);
        void setIdentity(fleece::slice certData, fleece::slice privateKeyData);

    protected:
        ~TLSContext();

    private:
        std::unique_ptr<sockpp::mbedtls_context> _context;
        fleece::Retained<crypto::Identity> _identity;
        role_t _role;

        friend class TCPSocket;
    };

} }
