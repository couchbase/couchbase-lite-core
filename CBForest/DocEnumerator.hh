//
//  DocEnumerator.hh
//  CBForest
//
//  Created by Jens Alfke on 6/18/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef CBForest_DocEnumerator_hh
#define CBForest_DocEnumerator_hh

#include "Document.hh"

namespace cbforest {

#if DEBUG
#define VALIDATE_ITERATOR 1
#endif

    /** KeyStore enumerator/iterator that returns a range of Documents.
        Usage:
            for (auto e=db.enumerate(); e.next(); ) {...}
        or
            auto e=db.enumerate();
            while (e.next()) { ... }
        Inside the loop you can treat the enumerator as though it were a Document*, for example
        "e->key()".
     */
    class DocEnumerator {
    public:
        struct Options {
            unsigned                 skip;
            unsigned                 limit;
            bool                     descending     :1;
            bool                     inclusiveStart :1;
            bool                     inclusiveEnd   :1;
            bool                     includeDeleted :1;
            KeyStore::contentOptions contentOptions :4;

            static const Options kDefault;
        };

        DocEnumerator(KeyStore&,
                      slice startKey = slice::null,
                      slice endKey = slice::null,
                      const Options& options = Options::kDefault);
        DocEnumerator(KeyStore&,
                      sequence start,
                      sequence end = UINT64_MAX,
                      const Options& options = Options::kDefault);
        DocEnumerator(KeyStore&,
                      std::vector<std::string> docIDs,
                      const Options& options = Options::kDefault);
        ~DocEnumerator();

        DocEnumerator(DocEnumerator&& e)    :_store(e._store) {*this = std::move(e);}
        DocEnumerator& operator=(DocEnumerator&& e); // move assignment

        /** Advances to the next key/document, returning false when it hits the end.
            next() must be called after the constructor before accessing the first document! */
        bool next();

        /** Repositions the enumerator at a specific key (or just after, if it's missing).
            You must call next() before accessing the document! */
        void seek(slice key);

        void close();

        const Document& doc() const         {return _doc;}

        /** Rvalue reference to document, allowing it to be moved (which will clear this copy) */
        Document&& moveDoc()                {return std::move(_doc);}

        // Can treat an enumerator as a document pointer:
        operator const Document*() const    {return _doc.key().buf ? &_doc : NULL;}
        const Document* operator->() const  {return _doc.key().buf ? &_doc : NULL;}

    protected:
        KeyStore *_store;
        fdb_iterator *_iterator {nullptr};
        Options _options;
        std::vector<std::string> _docIDs;
        int _curDocIndex {0};
        Document _doc;
        bool _skipStep {true};

#if VALIDATE_ITERATOR
        alloc_slice _minKey, _maxKey;
#endif

        friend class KeyStore;
        void setDocIDs(std::vector<std::string> docIDs);

        void freeDoc();

    private:
        DocEnumerator(KeyStore &store, const Options& options);
        DocEnumerator(const DocEnumerator&) = delete; // no copying allowed
        void initialPosition();
        bool nextFromArray();
        bool getDoc();
    };

}

#endif
