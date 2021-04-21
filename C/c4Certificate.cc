//
// c4Certificate.cc
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

#include "c4Database.hh"
#include "c4Certificate.hh"
#include "c4ReplicatorTypes.h"
#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "Logging.hh"
#include "NumConversion.hh"
#include <vector>

#if DEBUG
#ifndef ENABLE_CERT_REQUEST
#    define ENABLE_CERT_REQUEST 1
#endif

#ifndef ENABLE_SENDING_CERT_REQUESTS
#    define ENABLE_SENDING_CERT_REQUESTS 1
#endif
#endif

#if ENABLE_SENDING_CERT_REQUESTS
#include "CertRequest.hh"
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/pk.h"
#pragma clang diagnostic pop


#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::crypto;


CBL_CORE_API const C4CertIssuerParameters kDefaultCertIssuerParameters = {
    CertSigningRequest::kOneYear,
    C4STR("1"),
    -1,
    false,
    true,
    true,
    true
};


#pragma mark - C4CERT:


C4Cert::C4Cert(CertBase *impl)
:_impl(impl)
{ }


C4Cert::~C4Cert() = default;


Cert* C4Cert::asSignedCert() {
    return _impl->isSigned() ? (Cert*)_impl.get() : nullptr;
}


Cert* C4Cert::assertSignedCert() {
    AssertParam(_impl->isSigned(), "C4Certificate is not signed");
    return (Cert*)_impl.get();
}


CertSigningRequest* C4Cert::assertUnsignedCert() {
    AssertParam(!_impl->isSigned(), "C4Certificate is not a signing-request");
    return (CertSigningRequest*)_impl.get();
}


Retained<C4Cert> C4Cert::fromData(slice certData) {
    return new C4Cert(new Cert(certData));
}


alloc_slice C4Cert::data(bool pemEncoded) {
    return _impl->data(pemEncoded ? KeyFormat::PEM : KeyFormat::DER);
}


alloc_slice C4Cert::chainData() {
    if (auto signedCert = asSignedCert(); signedCert)
        return C4SliceResult(signedCert->dataOfChain());
    else
        return _impl->data(KeyFormat::PEM);
}


alloc_slice C4Cert::summary()       {return _impl->summary();}
alloc_slice C4Cert::subjectName()   {return _impl->subjectName();}
C4CertUsage C4Cert::usages()        {return _impl->nsCertType();}


alloc_slice C4Cert::subjectNameComponent(C4CertNameAttributeID attrID) {
    if (auto tag = SubjectAltNames::tagNamed(attrID); tag)
        return _impl->subjectAltNames()[*tag];
    else
        return _impl->subjectName()[attrID];
}


C4Cert::NameInfo C4Cert::subjectNameAtIndex(unsigned index) {
    // First go through the DistinguishedNames:
    auto subjectName = _impl->subjectName();
    if (auto dn = subjectName.asVector(); index < dn.size())
        return {alloc_slice(dn[index].first), alloc_slice(dn[index].second)};
    else
        index -= narrow_cast<unsigned>(dn.size());

    // Then look in SubjectAlternativeName:
    if (auto san = _impl->subjectAltNames(); index < san.size()) {
        return {alloc_slice(SubjectAltNames::nameOfTag(san[index].first)),
                alloc_slice(san[index].second)};
    }

    return {};
}


std::pair<C4Timestamp,C4Timestamp> C4Cert::validTimespan() {
    C4Timestamp created = 0, expires = 0;
    if (Cert *signedCert = asSignedCert(); signedCert) {
        time_t tCreated, tExpires;
        tie(tCreated, tExpires) = signedCert->validTimespan();
        created = C4Timestamp(difftime(tCreated, 0) * 1000.0);
        expires = C4Timestamp(difftime(tExpires, 0) * 1000.0);
    }
    return {created, expires};
}


bool C4Cert::isSelfSigned() {
    Cert *signedCert = asSignedCert();
    return signedCert && signedCert->isSelfSigned();
}


Retained<C4KeyPair> C4Cert::publicKey() {
    if (auto signedCert = asSignedCert(); signedCert)
        return new C4KeyPair(signedCert->subjectPublicKey().get());
    return nullptr;
}

Retained<C4KeyPair> C4Cert::loadPersistentPrivateKey() {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    if (auto key = assertSignedCert()->loadPrivateKey(); key)
        return new C4KeyPair(key);
    return nullptr;
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
}


Retained<C4Cert> C4Cert::nextInChain() {
    if (auto signedCert = asSignedCert(); signedCert)
        return new C4Cert(signedCert->next().get());
    return nullptr;
}


// Certificate signing requests:


Retained<C4Cert> C4Cert::createRequest(std::vector<C4CertNameComponent> nameComponents,
                                      C4CertUsage certUsages,
                                      C4KeyPair *subjectKey)
{
    vector<DistinguishedName::Entry> name;
    SubjectAltNames altNames;
    for (auto &component : nameComponents) {
        auto attributeID = component.attributeID;
        if (auto tag = SubjectAltNames::tagNamed(attributeID); tag)
        altNames.emplace_back(*tag, component.value);
        else
            name.push_back({attributeID, component.value});
    }
    Cert::SubjectParameters params(name);
    params.subjectAltNames = move(altNames);
    params.nsCertType = NSCertType(certUsages);
    return new C4Cert(new CertSigningRequest(params, subjectKey->privateKey()));
}

Retained<C4Cert> C4Cert::requestFromData(slice certRequestData) {
#if ENABLE_CERT_REQUEST
    return new C4Cert(new CertSigningRequest(certRequestData));
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "Certificate requests are disabled");
#endif
}


bool C4Cert::isSigned() {
    return _impl->isSigned();
}


void C4Cert::sendSigningRequest(const C4Address &address,
                        slice optionsDictFleece,
                        const SigningCallback &callback)
{
#if ENABLE_SENDING_CERT_REQUESTS
    auto internalCallback = [=](crypto::Cert *cert, C4Error error) {
        Retained<C4Cert> c4cert;
        if (cert)
            c4cert = new C4Cert(cert);
        callback(c4cert, error);
    };
    auto request = retained(new litecore::REST::CertRequest());
    request->start(assertUnsignedCert(),
                   net::Address(address),
                   AllocedDict(optionsDictFleece),
                   internalCallback);
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "Sending CSRs is disabled");
#endif
}


Retained<C4Cert> C4Cert::signRequest(const C4CertIssuerParameters &c4Params,
                                     C4KeyPair *issuerPrivateKey,
                                     C4Cert* C4NULLABLE issuerC4Cert)
{
    auto csr = assertUnsignedCert();
    auto privateKey = issuerPrivateKey->privateKey();
    AssertParam(privateKey != nullptr, "No private key");

    // Get the issuer cert:
    Cert *issuerCert = nullptr;
    if (issuerC4Cert) {
        issuerCert = issuerC4Cert->asSignedCert();
        AssertParam(issuerCert != nullptr,  "issuerCert is not signed");
    }

    // Construct the issuer parameters:
    CertSigningRequest::IssuerParameters params;
    params.validity_secs            = c4Params.validityInSeconds;
    params.serial                   = c4Params.serialNumber;
    params.max_pathlen              = c4Params.maxPathLen;
    params.is_ca                    = c4Params.isCA;
    params.add_authority_identifier = c4Params.addAuthorityIdentifier;
    params.add_subject_identifier   = c4Params.addSubjectIdentifier;
    params.add_basic_constraints    = c4Params.addBasicConstraints;

    // Sign!
    return new C4Cert(csr->sign(params, privateKey, issuerCert).get());
}


// Persistence:


void C4Cert::save(bool entireChain, slice name) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    assertSignedCert()->save(string(name), entireChain);
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
}


void C4Cert::deleteNamed(slice name) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    Cert::deleteCert(string(name));
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
}


Retained<C4Cert> C4Cert::load(slice name) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
        return new C4Cert(Cert::loadCert(string(name)).get());
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
}


#pragma mark - C4KEYPAIR:


C4KeyPair::C4KeyPair(Key *key)
:_impl(key)
{ }


C4KeyPair::~C4KeyPair() = default;


Retained<PublicKey> C4KeyPair::publicKey() {
    if (PrivateKey *priv = privateKey(); priv)
        return priv->publicKey();
    else
        return (PublicKey*)_impl.get();
}


PrivateKey* C4NULLABLE C4KeyPair::privateKey() {
    return _impl->isPrivate() ? (PrivateKey*)_impl.get() : nullptr;
}


PersistentPrivateKey* C4KeyPair::persistentPrivateKey() {
    if (PrivateKey *priv = privateKey(); priv)
        return priv->asPersistent();
    return nullptr;
}


Retained<C4KeyPair> C4KeyPair::generate(C4KeyPairAlgorithm algorithm,
                                        unsigned sizeInBits,
                                        bool persistent)
{
    AssertParam(algorithm == kC4RSA, "Invalid algorithm");
    Retained<PrivateKey> privateKey;
    if (persistent) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
        privateKey = PersistentPrivateKey::generateRSA(sizeInBits);
#else
        C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
    } else {
        privateKey = PrivateKey::generateTemporaryRSA(sizeInBits);
    }
    return new C4KeyPair(privateKey);
}


Retained<C4KeyPair> C4KeyPair::fromPublicKeyData(slice publicKeyData) {
    return new C4KeyPair(new PublicKey(publicKeyData));
}


Retained<C4KeyPair> C4KeyPair::fromPrivateKeyData(slice privateKeyData, slice passwordOrNull) {
    return new C4KeyPair(new PrivateKey(privateKeyData, passwordOrNull));
}


bool C4KeyPair::hasPrivateKey() {
    return privateKey() != nullptr;
}


alloc_slice C4KeyPair::publicKeyDigest() {
    return alloc_slice(_impl->digestString());
}


alloc_slice C4KeyPair::publicKeyData() {
    return _impl->publicKeyData();
}


alloc_slice C4KeyPair::privateKeyData() {
    if (auto priv = privateKey(); priv && priv->isPrivateKeyDataAvailable())
        return priv->privateKeyData();
    return nullslice;
}


// Persistence:


bool C4KeyPair::isPersistent() {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return persistentPrivateKey() != nullptr;
#else
    return false;
#endif
}


Retained<C4KeyPair> C4KeyPair::persistentWithPublicKey(C4KeyPair *c4Key) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    if (auto persistent = c4Key->persistentPrivateKey(); persistent)
        return new C4KeyPair(persistent);
    else if (auto privKey = PersistentPrivateKey::withPublicKey(c4Key->publicKey().get()); privKey)
        return new C4KeyPair(privKey.get());
    else
        return nullptr;
#else
    C4Error::raise(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support");
#endif
}


void C4KeyPair::removePersistent() {
    auto priv = privateKey();
    AssertParam(priv, "No private key");
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    if (auto persistentKey = priv->asPersistent(); persistentKey)
        persistentKey->remove();
#endif
}


#pragma mark - EXTERNAL KEYPAIR:


namespace litecore {

    class ExternalKeyPair : public ExternalPrivateKey {
    public:
        ExternalKeyPair(size_t keySizeInBits,
                        void *externalKey,
                        struct C4ExternalKeyCallbacks callbacks)
        :ExternalPrivateKey(unsigned(keySizeInBits))
        ,_externalKey(externalKey)
        ,_callbacks(callbacks)
        { }

    protected:
        virtual ~ExternalKeyPair() override {
            if (_callbacks.free)
                _callbacks.free(_externalKey);
        }

        virtual fleece::alloc_slice publicKeyDERData() override {
            alloc_slice data(_keyLength + 40);  // DER data is ~38 bytes longer than keyLength
            size_t len = data.size;
            if (!_callbacks.publicKeyData(_externalKey, (void*)data.buf, data.size, &len)) {
                WarnError("C4ExternalKey publicKeyData callback failed!");
                error::_throw(error::CryptoError, "C4ExternalKey publicKeyData callback failed");
            }
            Assert(len < data.size);
            data.resize(len);
            return data;
        }

        virtual fleece::alloc_slice publicKeyRawData() override {
            alloc_slice data( publicKeyDERData() );
            Retained<PublicKey> publicKey = new PublicKey(data);
            return publicKey->data(KeyFormat::Raw);
        }

        virtual int _decrypt(const void *input,
                             void *output,
                             size_t output_max_len,
                             size_t *output_len) noexcept override
        {
            if (!_callbacks.decrypt(_externalKey, C4Slice{input, _keyLength},
                                    output, output_max_len, output_len)) {
                WarnError("C4ExternalKey decrypt callback failed!");
                return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
            }
            return 0;
        }

        virtual int _sign(int/*mbedtls_md_type_t*/ digestAlgorithm,
                          fleece::slice inputData,
                          void *outSignature) noexcept override
        {
            if (!_callbacks.sign(_externalKey, C4SignatureDigestAlgorithm(digestAlgorithm),
                                 inputData, outSignature)) {
                WarnError("C4ExternalKey sign callback failed!");
                return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
            }
            return 0;
        }

    private:
        void* const                  _externalKey;
        C4ExternalKeyCallbacks const _callbacks;
    };

}


Retained<C4KeyPair> C4KeyPair::fromExternal(C4KeyPairAlgorithm algorithm,
                                            size_t keySizeInBits,
                                            void *externalKey,
                                            const C4ExternalKeyCallbacks &callbacks)
{
    AssertParam(algorithm == kC4RSA, "Invalid algorithm");
    return new C4KeyPair(new ExternalKeyPair(keySizeInBits, externalKey, callbacks));
}


#endif // COUCHBASE_ENTERPRISE
