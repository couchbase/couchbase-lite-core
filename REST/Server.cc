//
// Server.cc
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

#include "Server.hh"
#include "LWSResponder.hh"
#include "Request.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "c4ListenerInternal.hh"
#include "libwebsockets.h"
#include "LWSContext.hh"
#include <fnmatch.h>                //TODO: Windows support? (it is in POSIX...)


namespace litecore { namespace REST {
    using namespace std;
    using namespace litecore::websocket;


    Server::Server()
    { }


    void Server::setExtraHeaders(const std::map<std::string, std::string> &headers) {
        lock_guard<mutex> lock(_mutex);
        _extraHeaders = headers;
    }


    void Server::addHandler(Methods methods, const string &patterns, const Handler &handler) {
        lock_guard<mutex> lock(_mutex);
        split(patterns, "|", [&](const string &pattern) {
            _rules.push_back({methods, pattern, handler});
        });
    }


    Server::URIRule* Server::findRule(Method method, const string &path) {
        //lock_guard<mutex> lock(_mutex);       // called from dispatchResponder which locks
        for (auto &rule : _rules) {
            if ((rule.methods & method)
                    && 0 == fnmatch(rule.pattern.c_str(), path.c_str(), FNM_PATHNAME))
                return &rule;
        }
        return nullptr;
    }


    void Server::dispatchResponder(LWSResponder *rq) {
        lock_guard<mutex> lock(_mutex);
        try{
            string pathStr(rq->path());
            auto rule = findRule(rq->method(), pathStr);
            if (rule) {
                C4Log("Matched rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                rule->handler(*rq);
            } else if (nullptr != (rule = findRule(Methods::ALL, pathStr))) {
                C4Log("Wrong method for rule %s for path %s", rule->pattern.c_str(), pathStr.c_str());
                rq->respondWithStatus(HTTPStatus::MethodNotAllowed, "Method not allowed");
            } else {
                C4Log("No rule matched path %s", pathStr.c_str());
                rq->respondWithStatus(HTTPStatus::NotFound, "Not found");
            }
        } catch (const std::exception &x) {
            C4Warn("HTTP handler caught C++ exception: %s", x.what());
            rq->respondWithStatus(HTTPStatus::ServerError, "Internal exception");
        }
    }


    void Server::stop() {
        {
            lock_guard<mutex> lock(_mutex);
            _rules.clear();
        }
        LWSServer::stop();
    }

} }
