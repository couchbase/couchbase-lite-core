//
// Created by Callum Birks on 01/11/2022.
//

#ifndef LITECORE_SGCONNECTION_HH
#define LITECORE_SGCONNECTION_HH

#include "c4CppUtils.hh"
#include "HTTPTypes.hh"
#include <memory>

using namespace fleece;
using namespace litecore;
using namespace litecore::net;

struct SGConnection {
    C4Address address;
    C4String remoteDBName;
    alloc_slice authHeader { nullslice };
    alloc_slice pinnedCert { nullslice };
    std::shared_ptr<ProxySpec> proxy { nullptr };
    alloc_slice networkInterface { nullslice };
#ifdef COUCHBASE_ENTERPRISE
    c4::ref<C4Cert> remoteCert { nullptr };
    c4::ref<C4Cert> identityCert { nullptr };
    c4::ref<C4KeyPair> identityKey { nullptr };
#endif
};


#endif //LITECORE_SGCONNECTION_HH
