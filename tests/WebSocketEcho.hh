//
// WebSocketEcho.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
