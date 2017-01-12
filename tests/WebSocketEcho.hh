//
//  WebSocketEcho.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"

namespace litecore {

    class WebSocketEcho : public WebSocketDelegate {
    public:
        virtual void onConnect() override;
        virtual void onError(int errcode, const char *reason) override;
        virtual void onClose(int status, fleece::slice reason) override;
        virtual void onWriteable() override;
        virtual void onMessage(fleece::slice message, bool binary) override;

    private:
        int echo_count {10};
    };

}
