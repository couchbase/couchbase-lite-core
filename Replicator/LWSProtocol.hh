//
// LWSProtocol.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "c4Base.h"
#include "fleece/Fleece.hh"
#include <mutex>
#include <utility>

struct lws;

namespace litecore { namespace websocket {

    class LWSProtocol : public fleece::RefCounted {
    public:
        virtual ~LWSProtocol();

        virtual int dispatch(lws*, int callback_reason, void *user, void *in, size_t len);

    protected:
        virtual void onConnectionError(C4Error) =0;

        std::pair<int,std::string> decodeHTTPStatus();

        bool addRequestHeader(uint8_t* *dst, uint8_t *end,
                              const char *header, fleece::slice value);

        std::string getHeader(int /*lws_token_indexes*/ tokenIndex);
        std::string getHeaderFragment(int /*lws_token_indexes*/ tokenIndex, unsigned index);
        fleece::Doc encodeHTTPHeaders();

        C4Error getConnectionError(fleece::slice lwsErrorMessage);

        fleece::alloc_slice getCertPublicKey(fleece::slice certPEM);
        fleece::alloc_slice getPeerCertPublicKey();


        template <class BLOCK>
        void synchronized(BLOCK block) {
            std::lock_guard<std::mutex> _lock(_mutex);
            block();
        }


        std::mutex _mutex;                       // For synchronization
        ::lws* _client {nullptr};
    };

} }
