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

#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"
#include "c4Certificate.h"
#include "Certificate.hh"
#include "PublicKey.hh"

#ifdef COUCHBASE_ENTERPRISE

using namespace fleece;
using namespace litecore::crypto;
using namespace c4Internal;


static inline CertBase* internal(C4Cert *cert)    {return (CertBase*)cert;}
static inline C4Cert* external(CertBase *cert)    {return (C4Cert*)cert;}

template <class CERT>
auto retainedExternal(CERT *cert) {
    return external(retain(cert));
}

static CertSigningRequest* asUnsignedCert(C4Cert *cert, C4Error *outError =nullptr) {
    if (internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert already signed"_sl, outError);
        return nullptr;
    }
    return (CertSigningRequest*)cert;
}

static Cert* asSignedCert(C4Cert *cert, C4Error *outError =nullptr) {
    if (!internal(cert)->isSigned()) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Cert not signed"_sl, outError);
        return nullptr;
    }
    return (Cert*)cert;
}


struct C4Key : public RefCounted {
    Retained<PublicKey> publicKey;
    Retained<PrivateKey> privateKey;

    static C4Key* create(PublicKey *key) {
        C4Key *c4key = new C4Key;
        c4key->publicKey = key;
        return retain(c4key);
    }

    static C4Key* create(PrivateKey *key) {
        C4Key *c4key = create(key->publicKey());
        c4key->privateKey = key;
        return c4key;
    }
};


const C4CertIssuerParameters kDefaultCertIssuerParameters = {
    CertSigningRequest::kOneYear,
    C4STR("1"),
    -1,
    false,
    true,
    true,
    true
};


#pragma mark - C4CERT:


C4Cert* c4cert_createRequest(C4String subjectName,
                             C4CertUsage certUsages,
                             C4Key *subjectKey C4NONNULL,
                             C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        if (subjectName.size == 0) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Missing subjectName"_sl, outError);
            return nullptr;
        }
        Cert::SubjectParameters params(subjectName);
        params.ns_cert_type = certUsages;
        return retainedExternal(new CertSigningRequest(params, subjectKey->privateKey));
    });
}


C4Cert* c4cert_fromData(C4Slice certData) C4API {
    return tryCatch<C4Cert*>(nullptr, [&]() {
        return retainedExternal(new Cert(certData));
    });
}


C4SliceResult c4cert_copyData(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(internal(cert)->data());
    });
}


C4StringResult c4cert_subjectName(C4Cert* cert) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->subjectName());
    });
}


C4StringResult c4cert_summary(C4Cert* cert) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4StringResult(internal(cert)->summary());
    });
}


bool c4cert_isSigned(C4Cert* cert) C4API {
    return internal(cert)->isSigned();
}


C4Cert* c4cert_signRequest(C4Cert *c4Cert,
                           const C4CertIssuerParameters *c4Params,
                           C4Key *issuerPrivateKey,
                           C4Error *outError) C4API
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        auto csr = asUnsignedCert(c4Cert, outError);
        if (!csr)
            return nullptr;
        if (!issuerPrivateKey->privateKey) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "No private key"_sl, outError);
            return nullptr;
        }
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

        Retained<Cert> cert = csr->sign(params, issuerPrivateKey->privateKey);
        return retainedExternal(cert.get());
    });
}


bool c4cert_makePersistent(C4Cert* cert, C4Error *outError) {
    return tryCatch<bool>(outError, [&]() {
        if (auto signedCert = asSignedCert(cert); signedCert) {
            signedCert->makePersistent();
            return true;
        }
        return false;
    });
}


C4Key* c4cert_getPublicKey(C4Cert* cert) C4API {
    return tryCatch<C4Key*>(nullptr, [&]() -> C4Key* {
        if (auto signedCert = asSignedCert(cert); signedCert)
            return C4Key::create(signedCert->subjectPublicKey());
        return nullptr;
    });
}


C4Key* c4cert_loadPersistentPrivateKey(C4Cert* cert, C4Error *outError) C4API {
    return tryCatch<C4Key*>(outError, [&]() -> C4Key* {
        if (auto signedCert = asSignedCert(cert, outError); signedCert) {
            if (auto key = signedCert->loadPrivateKey(); key)
                return C4Key::create(key);
        }
        return nullptr;
    });
}


#pragma mark - C4KEY:


C4Key* c4key_createPair(C4KeyPairAlgorithm algorithm,
                        unsigned sizeInBits,
                        bool persistent,
                        C4Error *outError) C4API
{
    return tryCatch<C4Key*>(outError, [&]() -> C4Key* {
        if (algorithm != kC4RSA) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Invalid algorithm"_sl, outError);
            return nullptr;
        }
        Retained<PrivateKey> privateKey;
        if (persistent)
            privateKey = PersistentPrivateKey::generateRSA(sizeInBits);
        else
            privateKey = PrivateKey::generateTemporaryRSA(sizeInBits);
        return C4Key::create(privateKey);
    });
}


C4Key* c4key_fromPublicKeyData(C4Slice publicKeyData) C4API {
    return tryCatch<C4Key*>(nullptr, [&]() {
        return C4Key::create(new PublicKey(publicKeyData));
    });
}


C4Key* c4key_fromPrivateKeyData(C4Slice privateKeyData) C4API {
    return tryCatch<C4Key*>(nullptr, [&]() {
        return C4Key::create(new PrivateKey(privateKeyData));
    });
}


bool c4key_hasPrivateKey(C4Key* key) C4API {
    return key->privateKey != nullptr;
}


bool c4key_isPersistent(C4Key* key) C4API {
    return dynamic_cast<PersistentPrivateKey*>(key->privateKey.get()) != nullptr;
}


C4SliceResult c4key_publicKeyData(C4Key* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(key->publicKey->publicKeyData());
    });
}


C4SliceResult c4key_privateKeyData(C4Key* key) C4API {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        if (auto priv = key->privateKey; priv && priv->isPrivateKeyDataAvailable())
            return C4SliceResult(priv->privateKeyData());
        return C4SliceResult{};
    });
}


#endif // COUCHBASE_ENTERPRISE
