//
//  Document.cc
//  CBNano
//
//  Created by Jens Alfke on 11/11/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Document.hh"

using namespace std;

namespace cbforest {

    Document::Document(slice key)
    :Document()
    {
        setKey(key);
    }

    Document::Document(Document &&d)
    :_key(move(d._key)),
     _meta(move(d._meta)),
     _body(move(d._body)),
     _sequence(d._sequence),
     _deleted(d._deleted),
     _exists(d._exists)
    { }

    void Document::clearMetaAndBody() {
        setMeta(slice::null);
        setBody(slice::null);
        _sequence = 0;
        _exists = _deleted = false;
    }

    void Document::clear() {
        clearMetaAndBody();
        setKey(slice::null);
    }
    
    slice Document::resizeMeta(size_t newSize) {
        if (newSize != _meta.size) {
            void* newBuf = slice::reallocBytes((void*)_meta.buf, newSize);
            if (newBuf != _meta.buf) {
                _meta.dontFree();
                _meta = alloc_slice::adopt(newBuf, newSize);
            }
        }
        return meta();
    }

    Document Document::moveBody() {
        Document d(_key);               // copies key
        d._meta = _meta;                // copies meta
        d._body = move(_body);          // moves body, setting my _body to null
        d._sequence = _sequence;
        d._offset = _offset;
        d._deleted = _deleted;
        d._exists = _exists;
        return d;
    }

}
