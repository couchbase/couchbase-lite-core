//
// Certificate.cc
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

#include "Certificate.hh"
#include "Defer.hh"
#include "Logging.hh"
#include "Error.hh"
#include "StringUtil.hh"

#include "mbedUtils.hh"

#include "pk.h"
#include "x509_crt.h"
#include "x509_csr.h"
#include "md.h"
#include "error.h"

#include <stdlib.h>

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;


    Cert::Cert()
    :_cert(new mbedtls_x509_crt)
    {
        mbedtls_x509_crt_init(_cert.get());
    }


    Cert::~Cert()  {
        mbedtls_x509_crt_free(_cert.get());
    }


    Cert::Cert(slice data)
    :Cert()
    {
        Assert(data);
        TRY( mbedtls_x509_crt_parse(context(), (const uint8_t*)data.buf, data.size) );
        if (data.hasPrefix("-----"_sl)) {
            // Input is PEM, but _data should be the parsed DER:
            _data = alloc_slice(_cert->raw.p, _cert->raw.len);
        } else {
            _data = data;
        }
    }


    Cert::Cert(const SubjectParameters &subjectParams,
               const IssuerParameters &issuerParams,
               PrivateKey *keyPair)
    :Cert( create(subjectParams, keyPair->publicKey(), issuerParams, keyPair, nullptr) )
    { }


    alloc_slice Cert::create(const SubjectParameters &subjectParams,
                             PublicKey *subjectKey NONNULL,
                             const IssuerParameters &issuerParams,
                             PrivateKey *issuerKeyPair NONNULL,
                             Cert *issuerCert)
    {
        mbedtls_x509write_cert crt;
        mbedtls_mpi serial;

        mbedtls_x509write_crt_init( &crt );
        mbedtls_mpi_init(&serial );
        DEFER {
            mbedtls_x509write_crt_free(&crt );
            mbedtls_mpi_free(&serial );
        };

        Log("Signing X.509 cert for '%s'", subjectParams.subject_name.c_str());

        string issuer_name = issuerCert ? issuerCert->subjectName() : subjectParams.subject_name;

        // Format the dates:
        time_t now = time(nullptr) - 60;
        time_t exp = now + issuerParams.validity_secs;
        struct tm tmnb, tmna;
        char notBefore[20], notAfter[20];
        strftime(notBefore, sizeof(notBefore), "%Y%m%d%H%M%S", gmtime_r(&now, &tmnb));
        strftime(notAfter,  sizeof(notAfter),  "%Y%m%d%H%M%S", gmtime_r(&exp, &tmna));

        // Set certificate attributes:
        mbedtls_x509write_crt_set_subject_key(&crt, subjectKey->context());
        mbedtls_x509write_crt_set_issuer_key(&crt, issuerKeyPair->context());
        TRY( mbedtls_x509write_crt_set_subject_name(&crt, subjectParams.subject_name.c_str()) );
        TRY( mbedtls_x509write_crt_set_issuer_name(&crt, issuer_name.c_str()) );
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        TRY( mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter) );

        TRY( mbedtls_mpi_read_string(&serial, 10, issuerParams.serial.c_str()));
        TRY( mbedtls_x509write_crt_set_serial(&crt, &serial));

        if (issuerParams.add_basic_constraints)
            TRY( mbedtls_x509write_crt_set_basic_constraints(&crt, issuerParams.is_ca,
                                                             issuerParams.max_pathlen ));
        if (issuerParams.add_subject_identifier)
            TRY( mbedtls_x509write_crt_set_subject_key_identifier(&crt) );
        if (issuerParams.add_authority_identifier) {
            auto originalIssuer = crt.issuer_key;
            Retained<PublicKey> tempKey;
            if (mbedtls_pk_get_type(issuerKeyPair->context()) == MBEDTLS_PK_RSA_ALT) {
                // Workaround for https://github.com/ARMmbed/mbedtls/issues/2768
                tempKey = issuerKeyPair->publicKey();
                crt.issuer_key = tempKey->context();
            }
            TRY( mbedtls_x509write_crt_set_authority_key_identifier(&crt) );
            crt.issuer_key = originalIssuer;
        }
        if (subjectParams.key_usage != 0)
            TRY( mbedtls_x509write_crt_set_key_usage(&crt, subjectParams.key_usage ));
        if (subjectParams.ns_cert_type != 0)
            TRY( mbedtls_x509write_crt_set_ns_cert_type(&crt, subjectParams.ns_cert_type ));

        // Finally, sign and encode the certificate:
        return allocDER(4096, [&](uint8_t *data, size_t size) {
            return mbedtls_x509write_crt_der(&crt, data, size,
                                             mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }


    alloc_slice Cert::data(KeyFormat f) {
        switch (f) {
        case KeyFormat::DER:
            return _data;
        case KeyFormat::PEM:
            return convertToPEM(_data, "CERTIFICATE");
        default:
            throwMbedTLSError(MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        }
    }


    mbedtls_pk_context* Cert::keyContext() {
        return &_cert->pk;
    }


    std::string Cert::subjectName() {
        return getX509Name(&_cert->subject);
    }


    std::string Cert::info(const char *indent) {
        string str;
        str.resize(10000, '\0');
        int len = mbedtls_x509_crt_info((char*)str.data(), 10000, indent, _cert.get());
        Assert(len >= 0);
        str.resize(len);
        return str;
    }


    #if 0
        // NOTE: These factory functions are implemented in a per-platform source file such as
        // PublicKey+Apple.mm, because they need to call platform-specific APIs.

        void Cert::makePersistent() {
            ... platform specific code...
        }

        Retained<Cert> Cert::loadCert(PublicKey *subjectKey) {
            ... platform specific code...
        }
    #endif


#pragma mark - CERT SIGNING REQUEST:

    // CertSigningRequest's implementation is very much like Cert's, but with slightly different
    // mbedTLS type and function names ("_crt_" --> "_csr_")


    CertSigningRequest::CertSigningRequest()
    :_csr(new mbedtls_x509_csr)
    {
        mbedtls_x509_csr_init(_csr.get());
    }


    CertSigningRequest::CertSigningRequest(slice data)
    :CertSigningRequest()
    {
        Assert(data);
        TRY( mbedtls_x509_csr_parse(context(), (const uint8_t*)data.buf, data.size) );
        if (data.hasPrefix("-----"_sl)) {
            // Input is PEM, but _data should be the parsed DER:
            _data = alloc_slice(_csr->raw.p, _csr->raw.len);
        } else {
            _data = data;
        }
    }


    CertSigningRequest::CertSigningRequest(const Cert::SubjectParameters &params,
                                           PrivateKey *subjectKey)
    :CertSigningRequest(create(params, subjectKey))
    { }


    alloc_slice CertSigningRequest::create(const Cert::SubjectParameters &params,
                                           PrivateKey *subjectKey)
    {
        // (This is simply a subset of what Cert::create() does, but the fn names differ slightly.)
        mbedtls_x509write_csr csr;
        mbedtls_x509write_csr_init( &csr );
        DEFER { mbedtls_x509write_csr_free(&csr); };

        Log("Creating X.509 cert request for '%s'", params.subject_name.c_str());

        // Set certificate attributes:
        mbedtls_x509write_csr_set_key(&csr, subjectKey->context());
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        TRY( mbedtls_x509write_csr_set_subject_name(&csr, params.subject_name.c_str()) );

        if (params.key_usage != 0)
            TRY( mbedtls_x509write_csr_set_key_usage(&csr, params.key_usage ));
        if (params.ns_cert_type != 0)
            TRY( mbedtls_x509write_csr_set_ns_cert_type(&csr, params.ns_cert_type ));

        // Finally, encode the request:
        return allocDER(4096, [&](uint8_t *data, size_t size) {
            return mbedtls_x509write_csr_der(&csr, data, size,
                                             mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }


    CertSigningRequest::~CertSigningRequest() {
        mbedtls_x509_csr_free(_csr.get());
    }


    alloc_slice CertSigningRequest::data(KeyFormat f) {
        switch (f) {
        case KeyFormat::DER:
            return _data;
        case KeyFormat::PEM:
            return convertToPEM(_data, "CERTIFICATE REQUEST");
        default:
            throwMbedTLSError(MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        }
    }


    string CertSigningRequest::subjectName() {
        return getX509Name(&_csr->subject);
    }


    mbedtls_pk_context* CertSigningRequest::keyContext() {
        return &_csr->pk;
    }


    Retained<Cert> CertSigningRequest::sign(const Cert::IssuerParameters &issuerParams,
                                            PrivateKey *issuerKeyPair,
                                            Cert *issuerCert)
    {
        Cert::SubjectParameters subjectParams(subjectName());
        //FIX: mbedTLS doesn't have any API to get the key usage or NS cert type from a CSR.
//        subjectParams.key_usage = ??;
//        subjectParams.ns_cert_type = ??;
        return new Cert(Cert::create(subjectParams, subjectPublicKey(),
                                     issuerParams, issuerKeyPair, issuerCert));
    }

} }
