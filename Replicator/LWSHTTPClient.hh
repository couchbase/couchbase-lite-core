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

        LWSHTTPClient(Response&,
                      const C4Address &address,
                      const char *method NONNULL,
                      fleece::alloc_slice requestBody = {});

        C4Error run();

    protected:
        int dispatch(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;
        bool onSendHeaders();
        bool onWriteRequest();
        void onResponseAvailable();
        bool onDataAvailable();
        void onRead(fleece::slice data);
        void onCompleted();
        void waitTilFinished();
        void notifyFinished();

    private:
        Response& _response;
        fleece::alloc_slice _requestBody;
        fleece::slice _unsentBody;
        C4Error _error {};
        fleece::Writer _responseData;
        std::condition_variable _condition;
        bool _finished {false};
    };

} }
