//
// LWSHTTPClient.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "LWSProtocol.hh"
#include "Response.hh"
#include "c4Socket.h"
#include "fleece/Fleece.hh"
#include "Writer.hh"
#include <condition_variable>
#include <string>

namespace litecore { namespace net {

    /** An HTTP client connection. (The Response class presents a higher level interface.) */
    class LWSHTTPClient : public LWSProtocol {
    public:
        LWSHTTPClient() =default;

        void connect(REST::Response* NONNULL,
                     const C4Address &address,
                     const char *method NONNULL,
                     fleece::Doc headers,
                     fleece::alloc_slice requestBody = {});

        // blocks until finished
        C4Error run();

    protected:
        virtual ~LWSHTTPClient();
        void onEvent(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;
        void onSendHeaders(void *in, size_t len);
        void onWriteRequest();
        void onResponseAvailable();
        void onDataAvailable();
        void onRead(fleece::slice data);
        void onCompleted(int reason);
        void waitTilFinished();
        void notifyFinished();

        virtual const char *className() const noexcept override;

    private:
        fleece::Doc             _requestHeaders;
        REST::Response*         _response;
        C4Error                 _error {};
        fleece::Writer          _responseData;
        std::condition_variable _condition;
        bool                    _finished {false};
    };

} }
