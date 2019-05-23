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

namespace litecore { namespace net {

    /** Abstract base class of network connections based on libwebsockets.
        A wrapper around a `lws` object. */
    class LWSProtocol : public fleece::RefCounted {
    public:
        // Entry point for libwebsockets events.
        int _mainDispatch(lws*, int callback_reason, void *user, void *in, size_t len);

        // Used for logging: every subclass should override this to return its class name.
        virtual const char *className() const noexcept =0;

    protected:
        LWSProtocol() { }
        LWSProtocol(lws *connection);
        virtual ~LWSProtocol();

        virtual void dispatch(lws*, int callback_reason, void *user, void *in, size_t len);

        void setDispatchResult(int result)          {_dispatchResult = result;}
        
        bool check(int status);

        virtual void onDestroy()                    { }
        virtual void onConnectionError(C4Error) =0;

        std::pair<int,std::string> decodeHTTPStatus();

        bool addRequestHeader(uint8_t* *dst, uint8_t *end,
                              const char *header, fleece::slice value);
        bool addContentLengthHeader(uint8_t* *dst, uint8_t *end, uint64_t contentLength);

        bool hasHeader(int /*lws_token_indexes*/ tokenIndex);
        std::string getHeader(int /*lws_token_indexes*/ tokenIndex);
        std::string getHeaderFragment(int /*lws_token_indexes*/ tokenIndex, unsigned index);
        int64_t getContentLengthHeader();
        fleece::Doc encodeHTTPHeaders();

        C4Error getConnectionError(fleece::slice lwsErrorMessage);

        fleece::alloc_slice getCertPublicKey(fleece::slice certPEM);
        fleece::alloc_slice getPeerCertPublicKey();

        void callbackOnWriteable();

        // This function may be called from other threads
        void setDataToSend(fleece::alloc_slice);
        
        fleece::slice dataToSend() const        {return _unsent;}
        bool hasDataToSend() const              {return _unsent.size > 0;}
        void sendMoreData(bool asServer);

        template <class BLOCK>
        void synchronized(BLOCK block) {
            std::lock_guard<std::mutex> _lock(_mutex);
            block();
        }

        std::mutex _mutex;                       // For synchronization
        ::lws* _client {nullptr};
        int _dispatchResult;

    private:
        void clientCreated(::lws* client);

        fleece::alloc_slice _dataToSend;
        fleece::slice _unsent;

        friend class LWSContext;
    };

} }
