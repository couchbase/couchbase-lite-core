//
// XTLSSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "sockpp/stream_socket.h"
#include <string>
#include "mbedtls/ssl.h"

namespace litecore { namespace net {

    class tls_context {
    public:
        tls_context() =default;
        virtual ~tls_context() =default;

        int status() const                          {return _status;}
        operator bool() const                       {return _status == 0;}

        virtual void allowInvalidPeerCerts() =0;

        virtual std::unique_ptr<sockpp::stream_socket> wrapSocket(std::unique_ptr<sockpp::stream_socket>,
                                                                  const std::string &hostname) =0;

    protected:
        void setStatus(int s)                       {_status = s;}

    private:
        int _status =0;
    };


    class mbedtls_context : public tls_context {
    public:
        mbedtls_context(bool client =true);
        ~mbedtls_context() override;

        void allowInvalidPeerCerts() override;

        std::unique_ptr<sockpp::stream_socket> wrapSocket(std::unique_ptr<sockpp::stream_socket>,
                                                          const std::string &hostname) override;

        static std::string getSystemRootCertsPEM();

    private:
        mbedtls_ssl_config  _sslConfig;

        friend class mbedtls_socket;
    };

} }
