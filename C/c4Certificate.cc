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

#include "c4Database.h"
#include "c4Certificate.h"
#include "c4ReplicatorTypes.h"
#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"
#include "Certificate.hh"
#include "PublicKey.hh"
#include "Logging.hh"
#include "c4CppUtils.hh"
#include "NumConversion.hh"
#include <vector>

#undef ENABLE_SENDING_CERT_REQUESTS
#ifdef ENABLE_SENDING_CERT_REQUESTS
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


static inline CertBase* internal(C4Cert *cert)    {return (CertBase*)cert;}
static inline C4Cert* external(CertBase *cert)    {return (C4Cert*)cert;}

static CertSigningRequest* asUnsignedCert(C4Cert *cert NONNULL, C4Error *outError =nullptr) {
    if (internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert already signed"_sl, outError);
        return nullptr;
    }
    return (CertSigningRequest*)cert;
}

static Cert* asSignedCert(C4Cert *cert NONNULL, C4Error *outError =nullptr) {
    if (!internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert not signed"_sl, outError);
        return nullptr;
    }
    return (Cert*)cert;
}


static inline Key* internal(C4KeyPair *key)    {return (Key*)key;}
static inline C4KeyPair* external(Key *key)    {return (C4KeyPair*)key;}

LITECORE_UNUSED static PublicKey* publicKey(C4KeyPair *c4key NONNULL) {
    auto key = internal(c4key);
    return key->isPrivate() ? ((PrivateKey*)key)->publicKey().get() : (PublicKey*)key;
}

static PrivateKey* privateKey(C4KeyPair *c4key NONNULL) {
    auto key = internal(c4key);
    return key->isPrivate() ? (PrivateKey*)key : nullptr;
}

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
static PersistentPrivateKey* persistentPrivateKey(C4KeyPair *c4key NONNULL) {
    if (PrivateKey *priv = privateKey(c4key); priv)
        return priv->asPersistent();
    return nullptr;
}
#endif


template <class T>
static auto retainedExternal(T *ref)            {return external(retain(ref));}

template <class T>
static auto retainedExternal(Retained<T> &&ref) {return external(retain(std::move(ref)));}


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


C4Cert* c4cert_createRequest(const C4CertNameComponent *nameComponents,
                             size_t nameCount,
                             C4CertUsage certUsages,
                             C4KeyPair *subjectKey,
                             C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        vector<DistinguishedName::Entry> name;
        SubjectAltNames altNames;
        for (size_t i = 0; i < nameCount; ++i) {
            auto attributeID = nameComponents[i].attributeID;
            if (auto tag = SubjectAltNames::tagNamed(attributeID); tag)
                altNames.emplace_back(*tag, nameComponents[i].value);
            else
                name.push_back({attributeID, nameComponents[i].value});
        }
        Cert::SubjectParameters params(name);
        params.subjectAltNames = move(altNames);
        params.nsCertType = NSCertType(certUsages);
        return retainedExternal(new CertSigningRequest(params, privateKey(subjectKey)));
    });
}


C4Cert* c4cert_fromData(C4Slice certData, C4Error *outError) C4API {
    return tryCatch<C4Cert*>(outError, [&]() {
        return retainedExternal(new Cert(certData));
    });
}


C4Cert* c4cert_requestFromData(C4Slice certRequestData, C4Error *outError) C4API {
#ifdef ENABLE_CERT_REQUEST
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        return retainedExternal(new CertSigningRequest(certRequestData));
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented,
                   "Certificate requests are disabled"_sl, outError);
    return nullptr;
#endif
}


C4SliceResult c4cert_copyData(C4Cert* cert, bool pemEncoded) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(internal(cert)->data(pemEncoded ? KeyFormat::PEM : KeyFormat::DER));
    });
}


C4StringResult c4cert_subjectName(C4Cert* cert) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->subjectName());
    });
}


C4StringResult c4cert_subjectNameComponent(C4Cert* cert, C4CertNameAttributeID attrID) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        if (auto tag = SubjectAltNames::tagNamed(attrID); tag)
            return C4StringResult(internal(cert)->subjectAltNames()[*tag]);
        else
            return C4StringResult(internal(cert)->subjectName()[attrID]);
    });
}


bool c4cert_subjectNameAtIndex(C4Cert* cert,
                               unsigned index,
                               C4CertNameInfo *outInfo) C4API
{
    outInfo->id = {};
    outInfo->value = {};

    // First go through the DistinguishedNames:
    auto subjectName = internal(cert)->subjectName();
    if (auto dn = subjectName.asVector(); index < dn.size()) {
        outInfo->id = FLSlice_Copy(dn[index].first);
        outInfo->value = C4StringResult(dn[index].second);
        return true;
    } else {
        index -= narrow_cast<unsigned>(dn.size());
    }

    // Then look in SubjectAlternativeName:
    if (auto san = internal(cert)->subjectAltNames(); index < san.size()) {
        outInfo->id = FLSlice_Copy(SubjectAltNames::nameOfTag(san[index].first));
        outInfo->value = C4StringResult(alloc_slice(san[index].second));
        return true;
    }

    return false;
}


C4CertUsage c4cert_usages(C4Cert* cert) C4API {
    return internal(cert)->nsCertType();
}


C4StringResult c4cert_summary(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->summary());
    });
}


void c4cert_getValidTimespan(C4Cert* cert,
                             C4Timestamp *outCreated,
                             C4Timestamp *outExpires)
{
    try {
        if (Cert *signedCert = asSignedCert(cert); signedCert) {
            time_t tCreated, tExpires;
            tie(tCreated, tExpires) = signedCert->validTimespan();
            if (outCreated)
                *outCreated = C4Timestamp(difftime(tCreated, 0) * 1000.0);
            if (outExpires)
                *outExpires = C4Timestamp(difftime(tExpires, 0) * 1000.0);
            return;
        }
    } catch (...) { }
    
    if (outCreated)
        *outCreated = 0;
    if (outExpires)
        *outExpires = 0;
}


bool c4cert_isSigned(C4Cert* cert) C4API {
    return internal(cert)->isSigned();
}


bool c4cert_isSelfSigned(C4Cert* cert) C4API {
    Cert *signedCert = asSignedCert(cert);
    return signedCert && signedCert->isSelfSigned();
}


C4Cert* c4cert_signRequest(C4Cert *c4Cert,
                           const C4CertIssuerParameters *c4Params,
                           C4KeyPair *issuerPrivateKey,
                           C4Cert *issuerC4Cert,
                           C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        auto csr = asUnsignedCert(c4Cert, outError);
        if (!csr)
            return nullptr;
        if (!privateKey(issuerPrivateKey)) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "No private key"_sl, outError);
            return nullptr;
        }

        // Construct the issuer parameters:
        if (!c4Params)
            c4Params = &kDefaultCertIssuerParameters;
        CertSigningRequest::IssuerParameters params;
        params.validity_secs = c4Params->validityInSeconds;
        params.serial = c4Params->serialNumber;
        params.max_pathlen = c4Params->maxPathLen;
        params.is_ca = c4Params->isCA;
        params.add_authority_identifier = c4Params->addAuthorityIdentifier;
        params.add_subject_identifier = c4Params->addSubjectIdentifier;
        params.add_basic_constraints = c4Params->addBasicConstraints;

        // Get the issuer cert:
        Cert *issuerCert = nullptr;
        if (issuerC4Cert) {
            issuerCert = asSignedCert(issuerC4Cert);
            if (!issuerCert) {
                c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter,
                               "issuerCert is not signed"_sl, outError);
                return nullptr;
            }
        }

        // Sign!
        return retainedExternal(csr->sign(params, privateKey(issuerPrivateKey), issuerCert));
    });
}


bool c4cert_sendSigningRequest(C4Cert *c4Cert,
                               C4Address address,
                               C4Slice optionsDictFleece,
                               C4CertSigningCallback callback,
                               void *context,
                               C4Error *outError) C4API
{
#ifdef ENABLE_SENDING_CERT_REQUESTS
    auto csr = asUnsignedCert(c4Cert, outError);
    if (!csr)
        return false;
    return tryCatch(outError, [&] {
        auto request = retained(new litecore::REST::CertRequest());
        request->start(csr,
                       net::Address(address),
                       AllocedDict(optionsDictFleece),
                       [=](crypto::Cert *cert, C4Error error) {
                           callback(context, external(cert), error);
                       });
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "Sending CSRs is disabled"_sl, outError);
    return false;
#endif
}


C4KeyPair* c4cert_getPublicKey(C4Cert* cert) C4API {
    return tryCatch<C4KeyPair*>(nullptr, [&]() -> C4KeyPair* {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return retainedExternal(signedCert->subjectPublicKey());
        return nullptr;
    });
}


C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert* cert, C4Error *outError) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (auto signedCert = asSignedCert(cert, outError); signedCert) {
            if (auto key = signedCert->loadPrivateKey(); key)
                return retainedExternal(std::move(key));
        }
        return nullptr;
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return nullptr;
#endif
}


C4Cert* c4cert_nextInChain(C4Cert* cert) C4API {
    return tryCatch<C4Cert*>(nullptr, [&]() -> C4Cert* {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return retainedExternal(signedCert->next());
        return nullptr;
    });
}

C4SliceResult c4cert_copyChainData(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return C4SliceResult(signedCert->dataOfChain());
        else
            return c4cert_copyData(cert, true);
    });
}


bool c4cert_save(C4Cert *cert,
                 bool entireChain,
                 C4String name,
                 C4Error *outError)
{
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<bool>(outError, [&]() {
        if (cert) {
            if (auto signedCert = asSignedCert(cert, outError); signedCert) {
                signedCert->save(string(name), entireChain);
                return true;
            }
            return false;
        } else {
            Cert::deleteCert(string(name));
            return true;
        }
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return false;
#endif
}


C4Cert* c4cert_load(C4String name,
                    C4Error *outError)
{
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<C4Cert*>(outError, [&]() {
        return retainedExternal(Cert::loadCert(string(name)));
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return nullptr;
#endif
}


#pragma mark - C4KEYPAIR:


C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm,
                              unsigned sizeInBits,
                              bool persistent,
                              C4Error *outError) C4API
{
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (algorithm != kC4RSA) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Invalid algorithm"_sl, outError);
            return nullptr;
        }
        Retained<PrivateKey> privateKey;
        if (persistent) {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
            privateKey = PersistentPrivateKey::generateRSA(sizeInBits);
#else
            c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
            return nullptr;
#endif
        } else {
            privateKey = PrivateKey::generateTemporaryRSA(sizeInBits);
        }
        return retainedExternal(std::move(privateKey));
    });
}


C4KeyPair* c4keypair_fromPublicKeyData(C4Slice publicKeyData, C4Error *outError) C4API {
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return retainedExternal(new PublicKey(publicKeyData));
    });
}


C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice data, C4Slice password, C4Error *outError) C4API
{
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return retainedExternal(new PrivateKey(data, password));
    });
}


C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair* key, C4Error *outError) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (auto persistent = persistentPrivateKey(key); persistent != nullptr)
            return retainedExternal(persistent);
        auto privKey = PersistentPrivateKey::withPublicKey(publicKey(key));
        if (!privKey) {
            clearError(outError);
            return nullptr;
        }
        return retainedExternal(std::move(privKey));
    });
#else
    c4error_return(LiteCoreDomain, kC4ErrorUnimplemented, "No persistent key support"_sl, outError);
    return nullptr;
#endif
}


bool c4keypair_hasPrivateKey(C4KeyPair* key) C4API {
    return privateKey(key) != nullptr;
}


bool c4keypair_isPersistent(C4KeyPair* key) C4API {
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return persistentPrivateKey(key) != nullptr;
#else
    return false;
#endif
}


C4SliceResult c4keypair_publicKeyDigest(C4KeyPair* key) C4API {
    return toSliceResult(internal(key)->digestString());
}


C4SliceResult c4keypair_publicKeyData(C4KeyPair* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(internal(key)->publicKeyData());
    });
}


C4SliceResult c4keypair_privateKeyData(C4KeyPair* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        if (auto priv = privateKey(key); priv && priv->isPrivateKeyDataAvailable())
            return C4SliceResult(priv->privateKeyData());
        return C4SliceResult{};
    });
}


bool c4keypair_removePersistent(C4KeyPair* key, C4Error *outError) C4API {
    if (!privateKey(key)) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "No private key"_sl, outError);
        return false;
    }
#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
    return tryCatch(outError, [&]() {
        if (auto persistentKey = persistentPrivateKey(key); persistentKey)
            persistentKey->remove();
    });
#else
    return true;
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


C4KeyPair* c4keypair_fromExternal(C4KeyPairAlgorithm algorithm,
                                  size_t keySizeInBits,
                                  void *externalKey,
                                  C4ExternalKeyCallbacks callbacks,
                                  C4Error *outError)
{
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        if (algorithm != kC4RSA) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Invalid algorithm"_sl, outError);
            return nullptr;
        }
        return retainedExternal(new ExternalKeyPair(keySizeInBits, externalKey, callbacks));
    });
}



#endif // COUCHBASE_ENTERPRISE
