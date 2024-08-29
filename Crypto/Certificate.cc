//
// Certificate.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Certificate.hh"
#include "Defer.hh"
#include "Logging.hh"
#include "Error.hh"
#include "TempArray.hh"
#include "Writer.hh"
#include "slice_stream.hh"

#include "mbedSnippets.hh"
#include "mbedUtils.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/asn1write.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/platform.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include "date/date.h"

namespace litecore::crypto {
    using namespace std;
    using namespace std::chrono;
    using namespace std::chrono_literals;
    using namespace fleece;


#pragma mark - DISTINGUISHED NAME

    DistinguishedName::DistinguishedName(const std::vector<Entry>& entries) {
        Writer out;
        for ( auto& e : entries ) {
            if ( out.length() > 0 ) out << ", "_sl;

            out << e.key << '=';

            // Escape commas in the value:
            slice          value = e.value;
            const uint8_t* comma;
            while ( nullptr != (comma = value.findByte(',')) ) {
                out << slice(value.buf, comma) << R"(\,)"_sl;
                value.setStart(comma + 1);
            }
            out << value;
        }
        *this = DistinguishedName(out.finish());
    }

    DistinguishedName::VectorForm DistinguishedName::asVector() {
        VectorForm    result;
        slice_istream dn = slice(*this);
        while ( dn.size > 0 ) {
            slice key = dn.readToDelimiterOrEnd("="_sl);

            alloc_slice value;
            uint8_t     delim;
            do {
                auto next = dn.findAnyByteOf(R"(,\)"_sl);
                if ( next ) {
                    delim = *next;
                    value.append(slice(dn.buf, next));
                    if ( delim == '\\' ) {
                        ++next;
                        value.append(slice(next, 1));
                    }
                    dn.setStart(next + 1);
                } else {
                    value.append(dn);
                    dn = nullslice;
                    break;
                }
            } while ( delim != ',' );

            result.emplace_back(key, value);
            auto next = dn.findByteNotIn(" "_sl);
            if ( !next ) break;
            dn.setStart(next);
        }
        return result;
    }

    alloc_slice DistinguishedName::operator[](slice key) {
        for ( auto& kv : asVector() ) {
            if ( kv.first == key ) return kv.second;
        }
        return nullslice;
    }

#pragma mark - SUBJECT ALT NAME


    static const pair<slice, SANTag> kSANTagNames[] = {
            {"otherName"_sl, SANTag::kOtherName},
            {"rfc822Name"_sl, SANTag::kRFC822Name},
            {"dNSName"_sl, SANTag::kDNSName},
            {"x400Address"_sl, SANTag::kX400AddressName},
            {"directoryName"_sl, SANTag::kDirectoryName},
            {"ediPartyName"_sl, SANTag::kEDIPartyName},
            {"uniformResourceIdentifier"_sl, SANTag::kURIName},
            {"iPAddress"_sl, SANTag::kIPAddress},
            {"registeredID"_sl, SANTag::kRegisteredID},
    };

    std::optional<SANTag> SubjectAltNames::tagNamed(fleece::slice name) {
        for ( auto& item : kSANTagNames )
            if ( item.first == name ) return item.second;
        return nullopt;
    }

    fleece::slice SubjectAltNames::nameOfTag(SANTag tag) { return kSANTagNames[unsigned(tag)].first; }

    SubjectAltNames::SubjectAltNames(mbedtls_x509_sequence* subject_alt_names) {
        const mbedtls_x509_sequence* cur;
        for ( cur = subject_alt_names; cur; cur = cur->next ) {
            auto rawTag = cur->buf.tag;
            if ( (rawTag & MBEDTLS_ASN1_TAG_CLASS_MASK) != MBEDTLS_ASN1_CONTEXT_SPECIFIC ) continue;
            emplace_back(SANTag(rawTag & MBEDTLS_ASN1_TAG_VALUE_MASK), alloc_slice(cur->buf.p, cur->buf.len));
        }
        reverse(_names.begin(), _names.end());  // subject_alt_names list is in reverse order!
    }

    alloc_slice SubjectAltNames::encode() const {
        // Allocate enough buffer space:
        size_t bufferSize = 0;
        for ( auto& name : _names ) bufferSize += name.second.size + 16;
        TempArray(start, uint8_t, bufferSize);
        uint8_t* pos = start + bufferSize;

        size_t totalLen = 0;
        for ( auto& name : _names ) {
            size_t len = 0;
            len += TRY(mbedtls_asn1_write_raw_buffer(&pos, start, (const uint8_t*)name.second.buf, name.second.size));
            len += TRY(mbedtls_asn1_write_len(&pos, start, len));
            len += TRY(mbedtls_asn1_write_tag(&pos, start, MBEDTLS_ASN1_CONTEXT_SPECIFIC | uint8_t(name.first)));
            totalLen += len;
        }

        totalLen += TRY(mbedtls_asn1_write_len(&pos, start, totalLen));
        totalLen += TRY(mbedtls_asn1_write_tag(&pos, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
        return {pos, totalLen};
    }

    alloc_slice SubjectAltNames::operator[](SANTag tag) const {
        for ( auto& altName : _names ) {
            if ( altName.first == tag ) return altName.second;
        }
        return nullslice;
    }

#pragma mark - CERT BASE:

    alloc_slice CertBase::data(KeyFormat f) {
        switch ( f ) {
            case KeyFormat::DER:
                return alloc_slice(derData());
            case KeyFormat::PEM:
                return convertToPEM(derData(), isSigned() ? "CERTIFICATE" : "CERTIFICATE REQUEST");
            default:
                throwMbedTLSError(MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE);
        }
    }

    alloc_slice CertBase::summary(const char* indent) {
        return allocString(10000, [&](char* buf, size_t size) { return writeInfo(buf, size, indent); });
    }

#pragma mark - CERT:

    Cert::Cert(Cert* prev, mbedtls_x509_crt* crt) : _cert(crt), _prev(prev) {}

    Cert::Cert(slice data) : _cert((mbedtls_x509_crt*)mbedtls_calloc(1, sizeof(mbedtls_x509_crt))) {
        mbedtls_x509_crt_init(_cert);
        parsePEMorDER(data, "certificate", context(), &mbedtls_x509_crt_parse);
    }

    Cert::Cert(const SubjectParameters& subjectParams, const IssuerParameters& issuerParams, PrivateKey* keyPair)
        : Cert(create(subjectParams, keyPair->publicKey().get(), issuerParams, keyPair, nullptr)) {}

    Cert::~Cert() {
        if ( _prev ) {               // If I'm not the first:
            _prev->_next = nullptr;  // tell the previous cert I'm gone.
        } else {                     // Or if I'm the first:
            Assert(!_next);
            mbedtls_x509_crt_free(_cert);  // free the whole chain (except the memory of _cert)
            mbedtls_free(_cert);           // finally, free the memory of _cert
        }
    }

    // Given a set of cert-type flags, returns the key-usage flags that are required for a cert
    // of those type(s) to be valid.
    static uint8_t defaultKeyUsage(NSCertType certTypes, bool usingRSA) {
        // https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/nss_tech_notes/nss_tech_note3
        // NOTE: Modified for the more modern Diffie Hellman Ephemeral algorithms, which exercises
        // the digital signature usage on the server side
        uint8_t keyUsage = 0;
        if ( certTypes
             & (MBEDTLS_X509_NS_CERT_TYPE_SSL_CLIENT | MBEDTLS_X509_NS_CERT_TYPE_EMAIL
                | MBEDTLS_X509_NS_CERT_TYPE_OBJECT_SIGNING) )
            keyUsage |= MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
        if ( certTypes & (MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER | MBEDTLS_X509_NS_CERT_TYPE_EMAIL) )
            keyUsage |= ((usingRSA ? MBEDTLS_X509_KU_KEY_ENCIPHERMENT : MBEDTLS_X509_KU_KEY_AGREEMENT)
                         | MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        if ( certTypes
             & (MBEDTLS_X509_NS_CERT_TYPE_SSL_CA | MBEDTLS_X509_NS_CERT_TYPE_EMAIL_CA
                | MBEDTLS_X509_NS_CERT_TYPE_OBJECT_SIGNING_CA) )
            keyUsage |= MBEDTLS_X509_KU_KEY_CERT_SIGN;
        return keyUsage;
    }

    alloc_slice Cert::create(const SubjectParameters& subjectParams, PublicKey* subjectKey NONNULL,
                             const IssuerParameters& issuerParams, PrivateKey* issuerKeyPair NONNULL,
                             Cert* issuerCert) {
        {
            alloc_slice         issuerKeyData = issuerKeyPair->publicKeyData();
            Retained<PublicKey> issuerPublicKey;
            if ( issuerCert ) {
                if ( !issuerCert->_cert->ca_istrue ) error::_throw(error::InvalidParameter, "Issuer cert must be a CA");
                issuerPublicKey = issuerCert->subjectPublicKey();
            } else {
                issuerPublicKey = subjectKey;
            }
            if ( issuerKeyData != issuerPublicKey->publicKeyData() )
                error::_throw(error::InvalidParameter, "Issuer cert does not match issuer key");
        }

        mbedtls_x509write_cert crt;
        mbedtls_mpi            serial;

        mbedtls_x509write_crt_init(&crt);
        mbedtls_mpi_init(&serial);
        DEFER {
            mbedtls_x509write_crt_free(&crt);
            mbedtls_mpi_free(&serial);
        };

        string subjectName(subjectParams.subjectName);
        string issuerName = issuerCert ? string(issuerCert->subjectName()) : string(subjectParams.subjectName);
        LogTo(TLSLogDomain, "Signing X.509 cert for '%s', as issuer '%s'", subjectName.c_str(), issuerName.c_str());
        // Format the dates:
        auto         now = floor<seconds>(system_clock::now()) - 60s;
        auto         exp = now + seconds(issuerParams.validity_secs);
        stringstream notBefore, notAfter;
        notBefore << date::format("%Y%m%d%H%M%S", now);
        notAfter << date::format("%Y%m%d%H%M%S", exp);

        // Set certificate attributes:
        mbedtls_x509write_crt_set_subject_key(&crt, subjectKey->context());
        mbedtls_x509write_crt_set_issuer_key(&crt, issuerKeyPair->context());
        TRY(mbedtls_x509write_crt_set_subject_name(&crt, subjectName.c_str()));
        TRY(mbedtls_x509write_crt_set_issuer_name(&crt, issuerName.c_str()));
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        TRY(mbedtls_x509write_crt_set_validity(&crt, notBefore.str().c_str(), notAfter.str().c_str()));

        if ( !subjectParams.subjectAltNames.empty() ) {
            // Subject Alternative Name -- mbedTLS doesn't have high-level APIs for this
            alloc_slice ext      = subjectParams.subjectAltNames.encode();
            bool        critical = (subjectParams.subjectName.size == 0);
            TRY(mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                    MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), critical,
                                                    (const uint8_t*)ext.buf, ext.size));
        }

        TRY(mbedtls_mpi_read_string(&serial, 10, string(issuerParams.serial).c_str()));
        TRY(mbedtls_x509write_crt_set_serial(&crt, &serial));

        if ( issuerParams.add_basic_constraints )
            TRY(mbedtls_x509write_crt_set_basic_constraints(&crt, issuerParams.is_ca, issuerParams.max_pathlen));
        if ( issuerParams.add_subject_identifier ) TRY(mbedtls_x509write_crt_set_subject_key_identifier(&crt));
        if ( issuerParams.add_authority_identifier ) {
            auto                originalIssuer = crt.issuer_key;
            Retained<PublicKey> tempKey;
            if ( mbedtls_pk_get_type(issuerKeyPair->context()) == MBEDTLS_PK_RSA_ALT ) {
                // Workaround for https://github.com/ARMmbed/mbedtls/issues/2768
                tempKey        = issuerKeyPair->publicKey();
                crt.issuer_key = tempKey->context();
            }
            TRY(mbedtls_x509write_crt_set_authority_key_identifier(&crt));
            crt.issuer_key = originalIssuer;
        }

        unsigned key_usage = subjectParams.keyUsage;
        if ( subjectParams.nsCertType != 0 ) {
            TRY(mbedtls_x509write_crt_set_ns_cert_type(&crt, subjectParams.nsCertType));
            if ( key_usage == 0 )  // Set key usage based on cert type:
                key_usage = defaultKeyUsage(subjectParams.nsCertType, PublicKey::isRSA());
        }
        if ( key_usage != 0 ) TRY(mbedtls_x509write_crt_set_key_usage(&crt, key_usage));

        // Finally, sign and encode the certificate:
        return allocDER(4096, [&](uint8_t* data, size_t size) {
            return mbedtls_x509write_crt_der(&crt, data, size, mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }

    slice Cert::derData() { return {_cert->raw.p, _cert->raw.len}; }

    mbedtls_pk_context* Cert::keyContext() { return &_cert->pk; }

    DistinguishedName Cert::subjectName() { return DistinguishedName(getX509Name(&_cert->subject)); }

    unsigned Cert::keyUsage() { return _cert->key_usage; }

    NSCertType Cert::nsCertType() { return NSCertType(_cert->ns_cert_type); }

    SubjectAltNames Cert::subjectAltNames() { return SubjectAltNames(&_cert->subject_alt_names); }

    int Cert::writeInfo(char* buf, size_t bufSize, const char* indent) {
        return mbedtls_x509_crt_info(buf, bufSize, indent, _cert);
    }

    alloc_slice Cert::summary(const char* indent) {
        alloc_slice summary;
        for ( Retained<Cert> cert = this; cert; cert = cert->next() ) {
            alloc_slice single = cert->CertBase::summary(indent);
            if ( !summary ) summary = std::move(single);
            else {
                summary.append("----------------\n"_sl);
                summary.append(single);
            }
        }
        return summary;
    }

    static time_t x509_to_time_t(const mbedtls_x509_time& xtime) {
        date::sys_days    date     = date::year{xtime.year} / xtime.mon / xtime.day;
        date::sys_seconds datetime = date + (hours(xtime.hour) + minutes(xtime.min) + seconds(xtime.sec));

        // The limit of 32-bit time_t is approaching...
        auto result = datetime.time_since_epoch().count();
        if ( _usuallyFalse(result > numeric_limits<time_t>::max()) ) {
            Warn("time_t overflow, capping to max!");
            return numeric_limits<time_t>::max();
        }

        if ( _usuallyFalse(result < numeric_limits<time_t>::min()) ) {
            Warn("time_t underflow, capping to min!");
            return numeric_limits<time_t>::min();
        }

        return (time_t)result;
    }

    pair<time_t, time_t> Cert::validTimespan() {
        return {x509_to_time_t(_cert->valid_from), x509_to_time_t(_cert->valid_to)};
    }

    bool Cert::isSelfSigned() {
        return x509_name_cmp(&_cert->issuer, &_cert->subject) == 0
               && x509_crt_check_signature(_cert, _cert, nullptr) == 0;
    }

    bool Cert::hasChain() {
        // mbedTLS certs are chained as a linked list through their `next` pointers.
        return _cert->next != nullptr;
    }

    Retained<Cert> Cert::next() {
        if ( !_cert->next ) return nullptr;
        if ( _next ) return _next;
        Retained<Cert> newNext = new Cert(this, _cert->next);
        _next                  = newNext;
        return newNext;
    }

    void Cert::append(Cert* other) {
        Assert(!other->_prev);  // other must be the start of a chain (or standalone)
        if ( hasChain() ) {
            next()->append(other);
        } else {
            _cert->next  = other->_cert;
            _next        = other;
            other->_prev = this;
        }
    }

    alloc_slice Cert::dataOfChain() {
        if ( !hasChain() ) return data(KeyFormat::PEM);

        // Convert each cert to PEM:
        vector<alloc_slice> pems;
        size_t              totalSize = 0;
        for ( Retained<Cert> cert = this; cert; cert = cert->next() ) {
            alloc_slice pem = cert->data(KeyFormat::PEM);
            pems.push_back(pem);
            totalSize += pem.size;
        }

        // Concatenate the data:
        alloc_slice   result(totalSize);
        slice_ostream dst(result);
        for ( alloc_slice& pem : pems ) dst.write(pem);
        DebugAssert(dst.bytesWritten() == result.size);
        return result;
    }


#if 0
        // NOTE: These factory functions are implemented in a per-platform source file such as
        // PublicKey+Apple.mm, because they need to call platform-specific APIs.

        void Cert::save(const std::string &persistentID, bool entireChain) {
            ... platform specific code...
        }
        
        fleece::Retained<Cert> Cert::loadCert(const std::string &persistentID) {
            ... platform specific code...
        }
        
        void Cert::deleteCert(const std::string &persistentID) {
            ... platform specific code...
        }

        Retained<Cert> Cert::loadCert(PublicKey *subjectKey) {
            ... platform specific code...
        }
#endif


#pragma mark - CERT SIGNING REQUEST:

    // CertSigningRequest's implementation is very much like Cert's, but with slightly different
    // mbedTLS type and function names ("_crt_" --> "_csr_")


    CertSigningRequest::CertSigningRequest() : _csr(new mbedtls_x509_csr) { mbedtls_x509_csr_init(_csr.get()); }

    CertSigningRequest::CertSigningRequest(slice data) : CertSigningRequest() {
        parsePEMorDER(data, "certificate request", context(), &mbedtls_x509_csr_parse);
    }

    CertSigningRequest::CertSigningRequest(const Cert::SubjectParameters& params, PrivateKey* subjectKey)
        : CertSigningRequest(create(params, subjectKey)) {}

    alloc_slice CertSigningRequest::create(const Cert::SubjectParameters& params, PrivateKey* subjectKey) {
        // (This is simply a subset of what Cert::create() does, but the fn names differ slightly.)
        mbedtls_x509write_csr csr;
        mbedtls_x509write_csr_init(&csr);
        DEFER { mbedtls_x509write_csr_free(&csr); };

        string subjectName(params.subjectName);
        LogTo(TLSLogDomain, "Creating X.509 cert request for '%s'", subjectName.c_str());

        // Set certificate attributes:
        mbedtls_x509write_csr_set_key(&csr, subjectKey->context());
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        TRY(mbedtls_x509write_csr_set_subject_name(&csr, subjectName.c_str()));

        if ( !params.subjectAltNames.empty() ) {
            // Subject Alternative Name -- mbedTLS doesn't have high-level APIs for this
            alloc_slice ext = params.subjectAltNames.encode();
            TRY(mbedtls_x509write_csr_set_extension(&csr, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                    MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
                                                    (const uint8_t*)ext.buf, ext.size));
        }

        auto key_usage = params.keyUsage;
        if ( params.nsCertType != 0 ) {
            TRY(mbedtls_x509write_csr_set_ns_cert_type(&csr, params.nsCertType));
            if ( key_usage == 0 )  // Set key usage based on cert type:
                key_usage = defaultKeyUsage(params.nsCertType, PrivateKey::isRSA());
        }
        if ( key_usage != 0 ) TRY(mbedtls_x509write_csr_set_key_usage(&csr, uint8_t(key_usage)));

        // Finally, encode the request:
        return allocDER(4096, [&](uint8_t* data, size_t size) {
            return mbedtls_x509write_csr_der(&csr, data, size, mbedtls_ctr_drbg_random, RandomNumberContext());
        });
    }

    CertSigningRequest::~CertSigningRequest() { mbedtls_x509_csr_free(_csr.get()); }

    slice CertSigningRequest::derData() { return {_csr->raw.p, _csr->raw.len}; }

    DistinguishedName CertSigningRequest::subjectName() { return DistinguishedName(getX509Name(&_csr->subject)); }

    SubjectAltNames CertSigningRequest::subjectAltNames() { return SubjectAltNames(&_csr->subject_alt_names); }

    unsigned CertSigningRequest::keyUsage() { return _csr->key_usage; }

    NSCertType CertSigningRequest::nsCertType() { return NSCertType(_csr->ns_cert_type); }

    int CertSigningRequest::writeInfo(char* buf, size_t bufSize, const char* indent) {
        return mbedtls_x509_csr_info(buf, bufSize, indent, _csr.get());
    }

    mbedtls_pk_context* CertSigningRequest::keyContext() { return &_csr->pk; }

    Retained<Cert> CertSigningRequest::sign(const Cert::IssuerParameters& issuerParams, PrivateKey* issuerKeyPair,
                                            Cert* issuerCert) {
        Cert::SubjectParameters subjectParams(subjectName());
        subjectParams.keyUsage        = keyUsage();
        subjectParams.nsCertType      = nsCertType();
        subjectParams.subjectAltNames = subjectAltNames();
        auto cert                     = retained(new Cert(
                Cert::create(subjectParams, subjectPublicKey().get(), issuerParams, issuerKeyPair, issuerCert)));
        if ( issuerCert ) {
            auto issuerCopy = retained(new Cert(issuerCert->dataOfChain()));
            cert->append(issuerCopy);
        }
        return cert;
    }

    Identity::Identity(Cert* cert_, PrivateKey* key_) : cert(cert_), privateKey(key_) {
        // Make sure the private and public keys match:
        Assert(mbedtls_pk_check_pair(cert->subjectPublicKey()->context(), privateKey->context()) == 0);
    }

}  // namespace litecore::crypto
