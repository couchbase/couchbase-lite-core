//
// Request.cc
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

#include "Request.hh"
#include "Writer.hh"
#include "LWSContext.hh"
#include "PlatformIO.hh"
#include "Error.hh"
#include "civetUtils.hh"
#include <stdarg.h>

using namespace std;
using namespace fleece;

namespace litecore { namespace REST {

#pragma mark - REQUEST:


    Request::Request(Method method, string path, fleece::slice queries,
                     fleece::Doc headers, fleece::alloc_slice body)
    :Body(headers, body)
    ,_method(method)
    ,_path(path)
    ,_queries(queries)
    { }


    void Request::setRequest(Method method, string path, fleece::slice queries,
                             fleece::Doc headers, fleece::alloc_slice body)
    {
        setHeaders(headers);
        setBody(body);
        _method = method;
        _path = path;
        _queries = queries;
    }


    string Request::path(int i) const {
        slice path = _path;
        Assert(path[0] == '/');
        path.moveStart(1);
        for (; i > 0; --i) {
            auto slash = path.findByteOrEnd('/');
            if (slash == path.end())
                return "";
            path.setStart(slash + 1);
        }
        auto slash = path.findByteOrEnd('/');
        if (slash == path.buf)
            return "";
        auto component = slice(path.buf, slash).asString();
        return urlDecode(component);
    }

    
    string Request::query(const char *param) const {
        string result;
        if (_queries)
            litecore::REST::getParam((const char*)_queries.buf, _queries.size, param, result, 0);
        return result;
    }

    int64_t Request::intQuery(const char *param, int64_t defaultValue) const {
        string val = query(param);
        if (!val.empty()) {
            try {
                return stoll(val);
            } catch (...) { }
        }
        return defaultValue;
    }

    bool Request::boolQuery(const char *param, bool defaultValue) const {
        string val = query(param);
        if (val.empty())
            return defaultValue;
        return val != "false" && val != "0";        // same behavior as Obj-C CBL 1.x
    }

} }
