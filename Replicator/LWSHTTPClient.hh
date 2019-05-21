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
#include <string>

namespace litecore { namespace REST {

    class LWSHTTPClient : public websocket::LWSProtocol {
    public:

        LWSHTTPClient(Response&);

        void connect(const C4Address &address,
                     const char *method NONNULL,
                     fleece::Doc headers,
                     fleece::alloc_slice requestBody = {});

        // blocks until finished
        C4Error run();

        virtual const char *className() const noexcept override;

    protected:
        void dispatch(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;
        bool onSendHeaders(void *in, size_t len);
        bool onWriteRequest();
        void onResponseAvailable();
        void onDataAvailable();
        void onRead(fleece::slice data);
        void onCompleted(int reason);
        void waitTilFinished();
        void notifyFinished();

    private:
        fleece::Doc _requestHeaders;
        Response& _response;
        C4Error _error {};
        fleece::Writer _responseData;
        std::condition_variable _condition;
        bool _finished {false};
    };

} }
