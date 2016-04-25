//
//  Document.hh
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

#ifndef __CBForest__Document__
#define __CBForest__Document__

#include "Database.hh"

namespace cbforest {

    class DocEnumerator;

    /** Stores a document's key, metadata and body as slices. Memory is owned by the object and
        will be freed when it destructs. Setters copy, getters don't. */
    class Document {
    public:
        Document();
        Document(slice key);
        Document(Document&&);
        ~Document();

        slice key() const   {return slice(_doc.key, _doc.keylen);}
        slice meta() const  {return slice(_doc.meta, _doc.metalen);}
        slice body() const  {return slice(_doc.body, _doc.bodylen);}

        void setKey(slice key);
        void setMeta(slice meta);
        void setBody(slice body);

        slice resizeMeta(size_t);

        void clearMetaAndBody();

        cbforest::sequence sequence() const {return _doc.seqnum;}
        uint64_t offset() const     {return _doc.offset;}
        size_t sizeOnDisk() const   {return _doc.size_ondisk;}
        bool deleted() const        {return _doc.deleted;}
        bool exists() const         {return !_doc.deleted && _doc.keylen > 0
                                                    && (_doc.size_ondisk > 0 || _doc.offset > 0);}
        bool valid() const;

        void updateSequence(cbforest::sequence s)                 {_doc.seqnum = s;}

        typedef DocEnumerator enumerator;

        static const size_t kMaxKeyLength, kMaxMetaLength, kMaxBodyLength;
        
        operator fdb_doc*() {return &_doc;}

    private:
        friend class KeyStore;
        friend class KeyStoreWriter;
        friend class Transaction;

        Document(const Document&) = delete;
        Document& operator= (const Document&) = delete;

        fdb_doc _doc = {};
    };

}

#endif /* defined(__CBForest__Document__) */
