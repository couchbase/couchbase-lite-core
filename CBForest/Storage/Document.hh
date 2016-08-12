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

#ifndef __CBNano__Document__
#define __CBNano__Document__

#include "Base.hh"

namespace cbforest {

    using namespace std;


    /** Stores a CBForest document's key, metadata, body and sequence. */
    class Document {
    public:
        Document()                              { }
        explicit Document(slice key);
        Document(const Document&);
        Document(Document&&);
        Document& operator=(const Document&);

        const alloc_slice& key() const          {return _key;}
        const alloc_slice& meta() const         {return _meta;}
        const alloc_slice& body() const         {return _body;}

        size_t bodySize() const                 {return _bodySize;}

        sequence_t sequence() const             {return _sequence;}
        bool deleted() const                    {return _deleted;}

        /** A storage-system-dependent position in the database file, that can be used later
            to retrieve the document. Not supported by all storage systems. */
        uint64_t offset() const                 {return _offset;}

        bool exists() const                     {return _exists;}

        template <typename T>
            void setKey(const T &key)           {_key = key;}
        template <typename T>
            void setMeta(const T &meta)         {_meta = meta;}
        template <typename T>
            void setBody(const T &body)         {_body = body; _bodySize = _body.size;}

        // Sets key/meta/body from an existing malloc'ed block. The Document assumes responsibility
        // for freeing the block; caller should _not_ free it afterwards.
        void adoptKey(slice key)                {_key = alloc_slice::adopt(key);}
        void adoptMeta(slice meta)              {_meta = alloc_slice::adopt(meta);}
        void adoptBody(slice body)              {_body = alloc_slice::adopt(body); _bodySize = _body.size;}

        void setDeleted(bool deleted)           {_deleted = deleted; if (deleted) _exists = false;}

        /** Reallocs the 'meta' slice to the desired size. */
        const alloc_slice& resizeMeta(size_t newSize)        {_meta.resize(newSize); return _meta;}

        /** Clears/frees everything. */
        void clear();

        /** Clears everything but the key. */
        void clearMetaAndBody();

        void updateSequence(sequence_t s)       {_sequence = s;}
        void setUnloadedBodySize(size_t size)   {_body = slice::null; _bodySize = size;}

    private:
        friend class KeyStore;
        friend class KeyStoreWriter;
        friend class Transaction;
        friend class DocEnumerator;

        void update(sequence_t sequence, uint64_t offset, bool deleted) {
            _sequence = sequence; _offset = offset; _deleted = deleted; _exists = !deleted;
        }

        alloc_slice _key, _meta, _body;     // The key, metadata and body of the document
        size_t      _bodySize {0};          // Size of body, if body wasn't loaded
        sequence_t  _sequence {0};          // Sequence number (if KeyStore supports sequences)
        uint64_t    _offset {0};            // File offset in db, if KeyStore supports that
        bool        _deleted {false};       // Is the document deleted?
        bool        _exists {false};        // Does the document exist?
    };

}

#endif /* defined(__CBNano__Document__) */
