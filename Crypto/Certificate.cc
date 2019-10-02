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
#include "Writer.hh"

#include "mbedUtils.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#pragma clang diagnostic pop

#include <stdlib.h>

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;


#pragma mark - DISTINGUISHED NAME


    DistinguishedName::DistinguishedName(const Entry *begin,  const Entry *end) {
        Writer out;
        for (auto e = begin; e != end; ++e) {
            if (e != begin)
                out << ", "_sl;

            out << e->key << '=';

            // Escape commas in the value:
            slice value = e->value;
            const uint8_t *comma;
            while (nullptr != (comma = value.findByte(','))) {
                out << slice(value.buf, comma) << "\\,"_sl;
                value.setStart(comma + 1);
            }
            out << value;
        }
        *this = out.finish();
    }


    DistinguishedName DistinguishedName::parse(slice str) {
        return DistinguishedName(alloc_slice(str));
    }


    alloc_slice DistinguishedName::operator[] (slice key) {
        slice dn = *this;
        while (dn.size > 0) {
            slice foundKey = dn.readToDelimiterOrEnd("="_sl);
            bool found = (foundKey == key);

            alloc_slice value;
            uint8_t delim;
            do {
                auto next = dn.findAnyByteOf(",\\"_sl);
                if (next) {
                    delim = *next;
                    if (found)
                        value.append(slice(dn.buf, next));
                    if (delim == '\\') {
                        ++next;
                        if (found)
                            value.append(slice(next, 1));
                    }
                    dn.setStart(next + 1);
                } else {
                    if (found)
                        value.append(dn);
                    dn = nullslice;
                    break;
                }
            } while (delim != ',');

            if (found)
                return value;
            auto next = dn.findByteNotIn(" "_sl);
            if (next)
                dn.setStart(next);
        }
        return nullslice;
    }


#pragma mark - CERT BASE:


    int CertBase::parseData(fleece::slice data, void *context, ParseFn parse) {
        Assert(data);
        bool isPEM = data.hasPrefix("-----"_sl);
        alloc_slice adjustedData;
        if (isPEM && !data.hasSuffix('\0')) {
            // mbedTLS insists PEM data must end with a NUL byte, so add one:
            adjustedData = alloc_slice(data);
            adjustedData.resize(data.size + 1);
            *((char*)adjustedData.end() - 1) = '\0';
            data = adjustedData;
        }

        return TRY( parse(context, (const uint8_t*)data.buf, data.size) );
    }



    alloc_slice CertBase::data(KeyFormat f) {
        switch (f) {
            case KeyFormat::DER:
                return alloc_slice(derData());
            case KeyFormat::PEM:
                return convertToPEM(derData(), isSigned() ? "CERTIFICATE" : "CERTIFICATE REQUEST");
            default:
                throwMbedTLSError(MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        }
    }


    alloc_slice CertBase::summary(const char *indent) {
        return allocString(10000, [&](char *buf, size_t size) {
            return writeInfo(buf, size, indent);
        });
    }


#pragma mark - CERT:


    Cert::Cert(Cert *prev, mbedtls_x509_crt *crt)
    :_cert(crt)
    ,_prev(prev)
    { }



    Cert::Cert(slice data)
    :_cert((mbedtls_x509_crt*)mbedtls_calloc(1, sizeof(mbedtls_x509_crt)))
    {
        mbedtls_x509_crt_init(_cert);
        parseData(data, context(), (ParseFn)&mbedtls_x509_crt_parse);
    }


    Cert::Cert(const SubjectParameters &subjectParams,
               const IssuerParameters &issuerParams,
               PrivateKey *keyPair)
    :Cert( create(subjectParams, keyPair->publicKey(), issuerParams, keyPair, nullptr) )
    { }


    Cert::~Cert()  {
        if (_prev) {                        // If I'm not the first:
            _prev->_next = nullptr;         // tell the previous cert I'm gone.
        } else {                            // Or if I'm the first:
            Assert(!_next);
            mbedtls_x509_crt_free(_cert);   // free the whole chain (except the memory of _cert)
            mbedtls_free(_cert);            // finally, free the memory of _cert
        }
    }


    // Given a set of cert-type flags, returns the key-usage flags that are required for a cert
    // of those type(s) to be valid.
    static uint8_t defaultKeyUsage(uint8_t certTypes, bool usingRSA) {
        // https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/nss_tech_notes/nss_tech_note3
        uint8_t keyUsage = 0;
        if (certTypes & (MBEDTLS_X509_NS_CERT_TYPE_SSL_CLIENT | MBEDTLS_X509_NS_CERT_TYPE_EMAIL
                         | MBEDTLS_X509_NS_CERT_TYPE_OBJECT_SIGNING))
            keyUsage |= MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
        if (certTypes & (MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER | MBEDTLS_X509_NS_CERT_TYPE_EMAIL))
            keyUsage |= (usingRSA ? MBEDTLS_X509_KU_KEY_ENCIPHERMENT : MBEDTLS_X509_KU_KEY_AGREEMENT);
        if (certTypes & (MBEDTLS_X509_NS_CERT_TYPE_SSL_CA | MBEDTLS_X509_NS_CERT_TYPE_EMAIL_CA
                         | MBEDTLS_X509_NS_CERT_TYPE_OBJECT_SIGNING_CA))
            keyUsage |= MBEDTLS_X509_KU_KEY_CERT_SIGN;
        return keyUsage;
    }


    alloc_slice Cert::create(const SubjectParameters &subjectParams,
                             PublicKey *subjectKey NONNULL,
                             const IssuerParameters &issuerParams,
                             PrivateKey *issuerKeyPair NONNULL,
                             Cert *issuerCert)
    {
        {
            alloc_slice issuerKeyData = issuerKeyPair->publicKeyData();
            Retained<PublicKey> issuerPublicKey;
            if (issuerCert) {
                if(!issuerCert->_cert->ca_istrue)
                    error::_throw(error::InvalidParameter, "Issuer cert must be a CA");
                issuerPublicKey = issuerCert->subjectPublicKey();
            } else {
                issuerPublicKey =  subjectKey;
            }
            if (issuerKeyData != issuerPublicKey->publicKeyData())
                error::_throw(error::InvalidParameter, "Issuer cert does not match issuer key");
        }
        
        mbedtls_x509write_cert crt;
        mbedtls_mpi serial;

        mbedtls_x509write_crt_init( &crt );
        mbedtls_mpi_init(&serial );
        DEFER {
            mbedtls_x509write_crt_free(&crt );
            mbedtls_mpi_free(&serial );
        };

        string subjectName(subjectParams.subject_name);
        string issuerName = issuerCert ? string(issuerCert->subjectName())
                                       : string(subjectParams.subject_name);
        Log("Signing X.509 cert for '%s', as issuer '%s'", subjectName.c_str(), issuerName.c_str());
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
        TRY( mbedtls_x509write_crt_set_subject_name(&crt, subjectName.c_str()) );
        TRY( mbedtls_x509write_crt_set_issuer_name(&crt, issuerName.c_str()) );
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        TRY( mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter) );

        TRY( mbedtls_mpi_read_string(&serial, 10, string(issuerParams.serial).c_str()));
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

        auto key_usage = subjectParams.key_usage;
        if (subjectParams.ns_cert_type != 0) {
            TRY( mbedtls_x509write_crt_set_ns_cert_type(&crt, subjectParams.ns_cert_type ));
            if (key_usage == 0) // Set key usage based on cert type:
                key_usage = defaultKeyUsage(subjectParams.ns_cert_type,
                                            subjectKey->isRSA());
        }
        if (key_usage != 0)
            TRY( mbedtls_x509write_crt_set_key_usage(&crt, key_usage ));

        // Finally, sign and encode the certificate:
        return allocDER(4096, [&](uint8_t *data, size_t size) {
            return mbedtls_x509write_crt_der(&crt, data, size,
                                             mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }


    slice Cert::derData() {
        return {_cert->raw.p, _cert->raw.len};
    }


    mbedtls_pk_context* Cert::keyContext() {
        return &_cert->pk;
    }


    DistinguishedName Cert::subjectName() {
        return DistinguishedName(getX509Name(&_cert->subject));
    }


    int Cert::writeInfo(char *buf, size_t bufSize, const char *indent) {
        return mbedtls_x509_crt_info(buf, bufSize, indent, _cert);
    }


    alloc_slice Cert::summary(const char *indent) {
        alloc_slice summary;
        for (Retained<Cert> cert = this; cert; cert = cert->next()) {
            alloc_slice single = cert->CertBase::summary(indent);
            if (!summary)
                summary = move(single);
            else {
                summary.append("----------------\n"_sl);
                summary.append(single);
            }
        }
        return summary;
    }


    bool Cert::hasChain() {
        // mbedTLS certs are chained as a linked list through their `next` pointers.
        return _cert->next != nullptr;
    }


    Retained<Cert> Cert::next() {
        if (!_cert->next)
            return nullptr;
        if (_next)
            return _next;
        Retained<Cert> newNext = new Cert(this, _cert->next);
        _next = newNext;
        return newNext;
    }


    void Cert::append(Cert *other) {
        Assert(!other->_prev);          // other must be the start of a chain (or standalone)
        if (hasChain()) {
            next()->append(other);
        } else {
            _cert->next = other->_cert;
            _next = other;
            other->_prev = this;
        }
    }


    alloc_slice Cert::dataOfChain() {
        if (!hasChain())
            return data(KeyFormat::PEM);

        // Convert each cert to PEM:
        vector<alloc_slice> pems;
        size_t totalSize = 0;
        for (Retained<Cert> cert = this; cert; cert = cert->next()) {
            alloc_slice pem = cert->data(KeyFormat::PEM);
            pems.push_back(pem);
            totalSize += pem.size;
        }

        // Concatenate the data:
        alloc_slice result(totalSize);
        void *dst = (void*) result.buf;
        for (alloc_slice &pem : pems) {
            memcpy(dst, pem.buf, pem.size);
            dst = offsetby(dst, pem.size);
        }
        DebugAssert(dst == result.end());
        return result;
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
        parseData(data, context(), (ParseFn)&mbedtls_x509_csr_parse);
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

        string subjectName(params.subject_name);
        Log("Creating X.509 cert request for '%s'", subjectName.c_str());

        // Set certificate attributes:
        mbedtls_x509write_csr_set_key(&csr, subjectKey->context());
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        TRY( mbedtls_x509write_csr_set_subject_name(&csr, subjectName.c_str()) );

        auto key_usage = params.key_usage;
        if (params.ns_cert_type != 0) {
            TRY( mbedtls_x509write_csr_set_ns_cert_type(&csr, params.ns_cert_type ));
            if (key_usage == 0) // Set key usage based on cert type:
                key_usage = defaultKeyUsage(params.ns_cert_type,
                                            subjectKey->isRSA());
        }
        if (key_usage != 0)
            TRY( mbedtls_x509write_csr_set_key_usage(&csr, key_usage ));

        // Finally, encode the request:
        return allocDER(4096, [&](uint8_t *data, size_t size) {
            return mbedtls_x509write_csr_der(&csr, data, size,
                                             mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }


    CertSigningRequest::~CertSigningRequest() {
        mbedtls_x509_csr_free(_csr.get());
    }


    slice CertSigningRequest::derData() {
        return {_csr->raw.p, _csr->raw.len};
    }


    DistinguishedName CertSigningRequest::subjectName() {
        return DistinguishedName(getX509Name(&_csr->subject));
    }


    int CertSigningRequest::writeInfo(char *buf, size_t bufSize, const char *indent) {
        return mbedtls_x509_csr_info(buf, bufSize, indent, _csr.get());
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
        auto cert = retained(new Cert(Cert::create(subjectParams, subjectPublicKey(),
                                                   issuerParams, issuerKeyPair, issuerCert)));
        if (issuerCert) {
            auto issuerCopy = retained(new Cert(issuerCert->dataOfChain()));
            cert->append(issuerCopy);
        }
        return cert;
    }


    Identity::Identity(Cert* cert_, PrivateKey* key_)
    :cert(cert_)
    ,privateKey(key_)
    {
        // Make sure the private and public keys match:
        Assert(mbedtls_pk_check_pair(cert->subjectPublicKey()->context(), privateKey->context()) == 0);
    }

} }
