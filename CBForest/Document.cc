//
//  Document.cc
//  CBForest
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

namespace cbforest {

    const size_t Document::kMaxKeyLength  = FDB_MAX_KEYLEN;
    const size_t Document::kMaxMetaLength = FDB_MAX_METALEN;
    const size_t Document::kMaxBodyLength = FDB_MAX_BODYLEN;

    Document::Document() {
        memset(&_doc, 0, sizeof(_doc));
    }

    Document::Document(Document&& srcDoc)
    :_doc(srcDoc._doc)
    {
        // Copy key and meta so srcDoc remains minimally useful, but move body
        _doc.key = (void*)key().copy().buf;
        _doc.meta = (void*)meta().copy().buf;
        srcDoc._doc.body = NULL; // to prevent double-free
        srcDoc._doc.bodylen = 0;
    }

    Document::Document(slice key) {
        setKey(key);
    }

    Document::~Document() {
        key().free();
        meta().free();
        body().free();
    }

    bool Document::valid() const {
        return _doc.key != NULL && _doc.keylen > 0 && _doc.keylen <= kMaxKeyLength
            && _doc.metalen <= kMaxMetaLength && !(_doc.metalen != 0 && _doc.meta == NULL)
            && _doc.bodylen <= kMaxBodyLength && !(_doc.bodylen != 0 && _doc.body == NULL);
    }

    void Document::clearMetaAndBody() {
        setMeta(slice::null);
        setBody(slice::null);
        _doc.deleted = false;
        _doc.seqnum = 0;
        _doc.offset = 0;
        _doc.size_ondisk = 0;
    }

    static inline void _assign(void* &buf, size_t &size, slice s) {
        ::free(buf);
        buf = (void*)s.copy().buf;
        size = s.size;
    }

    void Document::setKey(slice key)   {_assign(_doc.key,  _doc.keylen,  key);}
    void Document::setMeta(slice meta) {_assign(_doc.meta, _doc.metalen, meta);}
    void Document::setBody(slice body) {_assign(_doc.body, _doc.bodylen, body);}

    slice Document::resizeMeta(size_t newSize) {
        if (newSize != _doc.metalen) {
            _doc.meta = slice::reallocBytes(_doc.meta, newSize);
            _doc.metalen = newSize;
        }
        return meta();
    }

}
