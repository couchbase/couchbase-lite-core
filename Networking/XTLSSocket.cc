//
// XTLSSocket.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "XTLSSocket.hh"
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mutex>
#include <chrono>

#include "FilePath.hh"

#ifdef TARGET_OS_OSX
#include <Security/Security.h>
#endif

#define log(FMT,...) fprintf(stderr, FMT "\n", ## __VA_ARGS__)


namespace litecore { namespace net {
    using namespace std;
    using namespace sockpp;

    static const char* kEntropyPersonalization = "sockpp";


    static int checkMbedRet(int ret, const char *fn) {
        if (ret != 0) {
            char msg[100];
            mbedtls_strerror(ret, msg, sizeof(msg));
            log("mbedtls error -0x%04X from %s: %s", -ret, fn, msg);
        }
        return ret;
    }


    class mbedtls_socket : public stream_socket {
    public:
        mbedtls_socket(unique_ptr<stream_socket>,
                       mbedtls_context&,
                       const std::string &hostname);
        ~mbedtls_socket();

        virtual ssize_t read(void *buf, size_t n) override;
        virtual ssize_t read_n(void *buf, size_t n) override;
        virtual bool read_timeout(const std::chrono::microseconds& to) override;
        virtual ssize_t write(const void *buf, size_t n) override;
        virtual ssize_t write_n(const void *buf, size_t n) override;
        virtual bool write_timeout(const std::chrono::microseconds& to) override;

    private:
        int checkMbedRet(int ret, const char *fn);
        bool handshake();
        int bio_send(const void*, size_t);
        int bio_recv_timeout(void*, size_t length, uint32_t timeout);

        unique_ptr<stream_socket> _base;
        mbedtls_context& _context;
        mbedtls_ssl_context _ssl;
        bool _open = false;
        std::chrono::microseconds _readTimeout, _writeTimeout;
    };



#pragma mark - CONTEXT:


    static mbedtls_ctr_drbg_context* randomContext() {
        static mbedtls_entropy_context  sEntropy;
        static mbedtls_ctr_drbg_context sRandomContext;

        once_flag once;
        call_once(once, []() {
            mbedtls_entropy_init( &sEntropy );
            mbedtls_ctr_drbg_init( &sRandomContext );
            int err = mbedtls_ctr_drbg_seed(&sRandomContext, mbedtls_entropy_func, &sEntropy,
                                            (const uint8_t *)kEntropyPersonalization,
                                            strlen(kEntropyPersonalization));
            assert(err == 0);
        });
        return &sRandomContext;
    }


    mbedtls_context::mbedtls_context(bool client) {
        mbedtls_ssl_config_init(&_sslConfig);
        mbedtls_ssl_conf_rng(&_sslConfig, mbedtls_ctr_drbg_random, randomContext());
        setStatus(mbedtls_ssl_config_defaults(&_sslConfig,
                                              (client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER),
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT));
        if (status() != 0)
            return;

        string systemCerts = getSystemRootCertsPEM();
        if (!systemCerts.empty()) {
            mbedtls_x509_crt caChain;
            mbedtls_x509_crt_init(&caChain);
            setStatus(checkMbedRet(mbedtls_x509_crt_parse(&caChain,
                                                          (const uint8_t*)systemCerts.c_str(),
                                                          systemCerts.size() + 1),
                                   "mbedtls_x509_crt_parse"));
            if (status() == 0)
                mbedtls_ssl_conf_ca_chain(&_sslConfig, &caChain, nullptr);
            mbedtls_x509_crt_free(&caChain);
            if (status() != 0)
                return;
        }
    }


    void mbedtls_context::allowInvalidPeerCerts() {
        mbedtls_ssl_conf_authmode(&_sslConfig, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }


    unique_ptr<stream_socket> mbedtls_context::wrapSocket(unique_ptr<stream_socket> base,
                                                          const string &hostname)
    {
        return unique_ptr<stream_socket>(new mbedtls_socket(move(base), *this, hostname));
    }


    mbedtls_context::~mbedtls_context() {
        mbedtls_ssl_config_free(&_sslConfig);
    }


#pragma mark - SOCKET:


    int mbedtls_socket::checkMbedRet(int ret, const char *fn) {
        if (ret != 0) {
            litecore::net::checkMbedRet(ret, fn);
            set_last_error(ret);
            reset(); // marks me as closed/invalid
            _base->close();
        }
        return ret;
    }


    mbedtls_socket::mbedtls_socket(unique_ptr<stream_socket> base,
                                   mbedtls_context &context,
                                   const string &hostname)
    :stream_socket(-999)
    ,_base(move(base))
    ,_context(context)
    {
        mbedtls_ssl_init(&_ssl);
        if (context.status() != 0) {
            set_last_error(context.status());
            return;
        }

        if (checkMbedRet(mbedtls_ssl_setup(&_ssl, &_context._sslConfig),
                         "mbedtls_ssl_setup"))
            return;
        if (!hostname.empty() && checkMbedRet(mbedtls_ssl_set_hostname(&_ssl, hostname.c_str()),
                                              "mbedtls_ssl_set_hostname"))
            return;

        mbedtls_ssl_set_bio(&_ssl, this,
                            [](void *ctx, const uint8_t *buf, size_t len) {
                                return ((mbedtls_socket*)ctx)->bio_send(buf, len); },
                            nullptr,
                            [](void *ctx, uint8_t *buf, size_t len, uint32_t timeout) {
                                return ((mbedtls_socket*)ctx)->bio_recv_timeout(buf, len, timeout); });

        int status;
        do {
            status = mbedtls_ssl_handshake(&_ssl);
        } while (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (checkMbedRet(status, "mbedtls_ssl_handshake"))
            return;

        uint32_t verifyFlags = mbedtls_ssl_get_verify_result(&_ssl);
        if (verifyFlags != 0) {
            char vrfy_buf[512];
            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof( vrfy_buf ), "  ! ", verifyFlags);
            log("Cert verify failed: %s", vrfy_buf );
            reset();
            return;
        }
    }


    mbedtls_socket::~mbedtls_socket() {
        mbedtls_ssl_free(&_ssl);
        reset(); // remove bogus file descriptor so base class won't call close() on it
    }


    int mbedtls_socket::bio_send(const void* buf, size_t length) {
        return (int) _base->write_n(buf, length);
    }


    int mbedtls_socket::bio_recv_timeout(void* buf, size_t length, uint32_t timeout) {
        _base->read_timeout(chrono::milliseconds(timeout));
        return (int) _base->read(buf, length);
    }


    ssize_t mbedtls_socket::read(void *buf, size_t length) {
        return mbedtls_ssl_read(&_ssl, (uint8_t*)buf, length);
    }

    ssize_t mbedtls_socket::read_n(void *buf, size_t length) {
        auto dst = (uint8_t*)buf;
        size_t total = 0;
        while (total < length) {
            ssize_t n = read(dst, length - total);
            if (n < 0)
                return total ? total : n;
            else if (n == 0)
                break;
            dst += n;
            total += n;
        }
        return total;
    }

    bool mbedtls_socket::read_timeout(const chrono::microseconds& to) {
        _readTimeout = to;
        return true;
    }

    ssize_t mbedtls_socket::write(const void *buf, size_t length) {
        return mbedtls_ssl_write(&_ssl, (const uint8_t*)buf, length);
    }

    ssize_t mbedtls_socket::write_n(const void *buf, size_t length) {
        auto src = (const uint8_t*)buf;
        size_t total = 0;
        while (total < length) {
            ssize_t n = write(src, length - total);
            if (n < 0)
                return total ? total : n;
            else if (n == 0)
                break;
            src += n;
            total += n;
        }
        return total;
    }

    bool mbedtls_socket::write_timeout(const chrono::microseconds& to) {
        _writeTimeout = to;
        return true;
    }




#pragma mark - PLATFORM SPECIFIC:


#if TARGET_OS_OSX

    // Read system root CA certs on macOS.
    // (Sadly, SecTrustCopyAnchorCertificates() is not available on iOS)
    string mbedtls_context::getSystemRootCertsPEM() {
        CFArrayRef roots;
        OSStatus err = SecTrustCopyAnchorCertificates(&roots);
        if (err)
            return {};
        CFDataRef pemData = nullptr;
        err =  SecItemExport(roots, kSecFormatPEMSequence, kSecItemPemArmour, nullptr, &pemData);
        CFRelease(roots);
        if (err)
            return {};
        string pem((const char*)CFDataGetBytePtr(pemData), CFDataGetLength(pemData));
        CFRelease(pemData);
        return pem;
    }

#elif !defined(_WIN32)

    // Read system root CA certs on Linux using OpenSSL's cert directory
    string mbedtls_context::getSystemRootCertsPEM() {
        static constexpr const char* kCertsDir  = "/etc/ssl/certs/";
        static constexpr const char* kCertsFile = "ca-certificates.crt";

        try {
            stringstream certs;
            char buf[1024];
            // Subroutine to append a file to the `certs` stream:
            auto readFile = [&](const FilePath &file) {
                ifstream in(file.path());
                char lastChar = '\n';
                while (in) {
                    in.read(buf, sizeof(buf));
                    auto n = in.gcount();
                    if (n > 0) {
                        certs.write(buf, n);
                        lastChar = buf[n-1];
                    }
                }
                if (lastChar != '\n')
                    certs << '\n';
            };

            // FIXME: This uses LiteCore's 'FilePath' class; make it independent
            FilePath certsDir(kCertsDir);
            if (certsDir.existsAsDir()) {
                FilePath certsFile(certsDir, kCertsFile);
                if (certsFile.exists()) {
                    // If there is a file containing all the certs, just read it:
                    readFile(certsFile);
                } else {
                    // Otherwise concatenate all the certs found in the dir:
                    certsDir.forEachFile([&](const FilePath &file) {
                        string ext = file.extension();
                        if (ext == ".pem" || ext == ".crt")
                            readFile(file);
                    });
                }
                Log("Read system root certificates");
            }
            return certs.str();

        } catch (const exception &x) {
            LogError("C++ exception reading system root certificates: %s", x.what());
            return "";
        }
    }

#else
    string mbedtls_context::getSystemRootCertsPEM() {
        return "";
    }
#endif


} }
