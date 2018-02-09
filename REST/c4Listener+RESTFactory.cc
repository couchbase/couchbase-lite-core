//
// c4Listener+Factory.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "c4ListenerInternal.hh"
#include "RESTListener.hh"

namespace litecore { namespace REST {

    const C4ListenerAPIs kListenerAPIs = kC4RESTAPI;

    Listener* NewListener(const C4ListenerConfig *config) {
        if (config->apis == kC4RESTAPI)
            return new RESTListener(*config);
        else
            return nullptr;
    }

} }
