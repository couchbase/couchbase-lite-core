//
// c4Certificate.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4Base.hh"
#include "c4CertificateTypes.h"
#include <functional>
#include <vector>


C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************

struct C4Cert final : public fleece::RefCounted,
                      public fleece::InstanceCountedIn<C4Cert>,
                      C4Base
{
#ifdef COUCHBASE_ENTERPRISE
    static Retained<C4Cert> fromData(slice certData);

    alloc_slice getData(bool pemEncoded);

    alloc_slice getChainData();

    alloc_slice getSummary();

    alloc_slice getSubjectName();

    alloc_slice getSubjectNameComponent(C4CertNameAttributeID);

    struct NameInfo {
        alloc_slice id;   ///< X.509 attribute name (e.g. "CN" or "O"), like a C4CertNameAttributeID
        alloc_slice value;///< The value of the name component, i.e. the name.
    };

    NameInfo getSubjectNameAtIndex(unsigned index);
#endif // COUCHBASE_ENTERPRISE
    std::pair<C4Timestamp,C4Timestamp> getValidTimespan();
#ifdef COUCHBASE_ENTERPRISE
    C4CertUsage getUsages();

    bool isSelfSigned();

    Retained<C4KeyPair> getPublicKey();

    Retained<C4KeyPair> loadPersistentPrivateKey();

    Retained<C4Cert> getNextInChain();

    // Certificate signing requests:

    static Retained<C4Cert> createRequest(std::vector<C4CertNameComponent> nameComponents,
                                          C4CertUsage certUsages,
                                          C4KeyPair *subjectKey);

    static Retained<C4Cert> requestFromData(slice certRequestData);

    bool isSigned();

    using SigningCallback = std::function<void(C4Cert*,C4Error)>;

    void sendSigningRequest(const C4Address &address,
                            slice optionsDictFleece,
                            const SigningCallback &callback);

    Retained<C4Cert> signRequest(const C4CertIssuerParameters &params,
                                 C4KeyPair *issuerPrivateKey,
                                 C4Cert* C4NULLABLE issuerCert);

    // Persistence:

    void save(bool entireChain, slice name);

    static void deleteNamed(slice name);

    static Retained<C4Cert> load(slice name);

    static bool exists(slice name);

    // Internal:

    litecore::crypto::Cert* assertSignedCert();

private:
    explicit C4Cert(litecore::crypto::CertBase*);
    ~C4Cert();
    litecore::crypto::CertSigningRequest* assertUnsignedCert();

#endif // COUCHBASE_ENTERPRISE
    litecore::crypto::Cert* C4NULLABLE asSignedCert();
    Retained<litecore::crypto::CertBase> _impl;
};

#ifdef COUCHBASE_ENTERPRISE

#pragma mark - KEY PAIRS:


struct C4KeyPair final : public fleece::RefCounted, C4Base {

    static Retained<C4KeyPair> generate(C4KeyPairAlgorithm algorithm,
                                        unsigned sizeInBits,
                                        bool persistent);

    static Retained<C4KeyPair> fromPublicKeyData(slice publicKeyData);

    static Retained<C4KeyPair> fromPrivateKeyData(slice privateKeyData,
                                                  slice passwordOrNull);

    bool hasPrivateKey();

    alloc_slice getPublicKeyDigest();

    alloc_slice getPublicKeyData();

    alloc_slice getPrivateKeyData();

    // Persistence:

    bool isPersistent();

    static Retained<C4KeyPair> persistentWithPublicKey(C4KeyPair*);

    void removePersistent();

    // Externally-Implemented Key-Pairs:

    static Retained<C4KeyPair> fromExternal(C4KeyPairAlgorithm algorithm,
                                            size_t keySizeInBits,
                                            void *externalKey,
                                            const C4ExternalKeyCallbacks &callbacks);

    // Internal:
    litecore::crypto::PrivateKey* C4NULLABLE getPrivateKey();

private:
    friend struct C4Cert;

    explicit C4KeyPair(litecore::crypto::Key*);
    ~C4KeyPair();
    Retained<litecore::crypto::PublicKey> getPublicKey();
    litecore::crypto::PersistentPrivateKey* getPersistentPrivateKey();

    Retained<litecore::crypto::Key> _impl;
};

#endif // COUCHBASE_ENTERPRISE

C4_ASSUME_NONNULL_END
