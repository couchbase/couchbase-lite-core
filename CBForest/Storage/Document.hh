//
//  Document.hh
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

#ifndef __CBNano__Document__
#define __CBNano__Document__

#include "Base.hh"

namespace cbforest {

    using namespace std;


    /** Stores a document's key, metadata, body and sequence. Memory is owned by the object. */
    class Document {
    public:
        Document()                              { }
        Document(slice key);
        Document(Document&&);

        /** Returns a Document whose key and meta are copies, but which adopts this instance's body.
            Side effect is that this instance's body is set to null. */
        Document moveBody();

        slice key() const                       {return _key;}
        slice meta() const                      {return _meta;}
        slice body() const                      {return _body;}

        cbforest::sequence sequence() const       {return _sequence;}
        bool deleted() const                    {return _deleted;}

        bool exists() const                     {return _exists;}
        bool valid() const;

        void setKey(slice key)                  {_key = key;}
        void setMeta(slice meta)                {_meta = meta;}
        void setBody(slice body)                {_body = body;}

        void adoptKey(slice key)                {_key = alloc_slice::adopt(key);}
        void adoptMeta(slice meta)              {_meta = alloc_slice::adopt(meta);}
        void adoptBody(slice body)              {_body = alloc_slice::adopt(body);}

        void setKeyNoCopy(slice key)            {adoptKey(key); _key.dontFree();}
        void setMetaNoCopy(slice meta)          {adoptMeta(meta); _meta.dontFree();}
        void setBodyNoCopy(slice body)          {adoptBody(body); _body.dontFree();}

        void setDeleted(bool deleted)           {_deleted = deleted; if (deleted) _exists = false;}

        /** Reallocs the 'meta' slice to the desired size. */
        slice resizeMeta(size_t);

        /** Clears/frees everything. */
        void clear();

        /** Clears everything but the key. */
        void clearMetaAndBody();

        uint64_t offset() const                 {return _offset;}

        void updateSequence(cbforest::sequence s)         {_sequence = s;}

    private:
        friend class KeyStore;
        friend class KeyStoreWriter;
        friend class Transaction;
        friend class DocEnumerator;

        void update(cbforest::sequence sequence, uint64_t offset, bool deleted) {
            _sequence = sequence; _offset = offset; _deleted = deleted; _exists = !deleted;
        }

        Document(const Document&) = delete;                 // no copying allowed
        Document& operator=(const Document&) = delete;      // no assignment allowed

        alloc_slice _key, _meta, _body;
        cbforest::sequence _sequence    {0};
        uint64_t _offset                {0};
        bool _deleted                   {false};
        bool _exists                    {false};
    };

}

#endif /* defined(__CBNano__Document__) */
