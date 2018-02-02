//
// WebSocketEcho.cc
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

#include "WebSocketEcho.hh"

using namespace std;
using namespace fleece;

namespace litecore {

    
#pragma mark - TEST


    void WebSocketEcho::onConnect() {
        fprintf(stderr, "** Connected!\n");
        connection()->send(slice("hello"), false);
    }

    void WebSocketEcho::onError(int errcode, const char *reason) {
        fprintf(stderr, "** Error! %s (%d)\n", reason, errcode);
        connection()->provider().close();
    }

    void WebSocketEcho::onClose(int status, slice reason) {
        fprintf(stderr, "** Closing with status %d\n", status);
        connection()->provider().close();
    }

    void WebSocketEcho::onMessage(slice message, bool binary) {
        fprintf(stderr, ">> Message %d: \"%s\"\n", echo_count, ((string)message).c_str());
        echo_count--;

        {
            const char *send_msg = ((echo_count % 2) == 0) ? "Hello" : "World";
            connection()->send(slice(send_msg), false);
        }

        if (echo_count <= 0) {
            printf("** Got last echo\n");
            connection()->close();
        }
    }

    void WebSocketEcho::onWriteable() {
        fprintf(stderr, "** Ready to write\n");
    }

}
