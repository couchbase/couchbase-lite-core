//
//  DocEnumerator.hh
//  CBNano
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

#ifndef CBNano_DocEnumerator_hh
#define CBNano_DocEnumerator_hh

#include "Document.hh"
#include <vector>

namespace cbforest {

    using namespace std;

    class KeyStore;

    enum ContentOptions {
        kDefaultContent = 0,
        kMetaOnly = 0x01
    };

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
            unsigned                 skip;              //< Number of results to skip
            unsigned                 limit;             //< Max number of results to return
            bool                     descending     :1; //< Reverse order? (Start must be >= end)
            bool                     inclusiveStart :1; //< Include the start key/seq?
            bool                     inclusiveEnd   :1; //< Include the end key/seq?
            bool                     includeDeleted :1; //< Include deleted documents?
            ContentOptions           contentOptions :4; //< Load document bodies?

            /** Default options have inclusiveStart, inclusiveEnd, and include bodies. */
            static const Options kDefault;

            bool inclusiveMin() const {return descending ? inclusiveEnd : inclusiveStart;}
            bool inclusiveMax() const {return descending ? inclusiveStart : inclusiveEnd;}

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
                      vector<string> docIDs,
                      const Options& options = Options::kDefault);
        ~DocEnumerator();

        DocEnumerator(DocEnumerator&& e)    :_store(e._store) {*this = move(e);}
        DocEnumerator& operator=(DocEnumerator&& e); // move assignment

        /** Advances to the next key/document, returning false when it hits the end.
            next() must be called *before* accessing the first document! */
        bool next();

        bool atEnd() const                  {return _doc.key().buf == nullptr;}

        /** Repositions the enumerator at a specific key (or just after, if it's missing).
            You must call next() before accessing the document! */
        void seek(slice key);

        /** Stops the enumerator and frees its resources. (You only need to call this if the
            destructor might not be called soon enough.) */
        void close();

        /** The current document. */
        const Document& doc() const         {return _doc;}

        /** Rvalue reference to document, allowing it to be moved (which will clear this copy) */
        Document moveDoc()                  {return _doc.moveBody();}

        // Can treat an enumerator as a document pointer:
        operator const Document*() const    {return _doc.key().buf ? &_doc : NULL;}
        const Document* operator->() const  {return _doc.key().buf ? &_doc : NULL;}

        class Impl {
        public:
            virtual ~Impl()                         { }
            virtual bool next() =0;
            virtual bool seek(slice key) =0;
            virtual bool read(Document&) =0;
            virtual bool shouldSkipFirstStep()      {return false;}
        protected:
            void updateDoc(Document &doc, sequence s, uint64_t offset =0, bool exists =true) const {
                doc.update(s, offset, exists);
            }
        };

        DocEnumerator(const DocEnumerator&) = delete;               // no copying allowed
        DocEnumerator& operator=(const DocEnumerator&) = delete;    // no assignment allowed

        // C++11 'for' loop support
        // Allows a DocEnumerator `e` to be used like:
        //    for (const Document &doc : e) { ... }
        struct Iter {
            DocEnumerator* const _enum;
            Iter& operator++ ()             {_enum->next(); return *this;}
            operator const Document* ()     {return _enum ? (const Document*)(*_enum) : nullptr;}
        };
        Iter begin()    {next(); return Iter{this};}
        Iter end()      {return Iter{nullptr};}

    private:
        friend class KeyStore;

        DocEnumerator(KeyStore &store, const Options& options);
        void setDocIDs(vector<string> docIDs);
        void initialPosition();
        bool nextFromArray();
        bool getDoc();

        KeyStore *_store;
        Options _options;
        vector<string> _docIDs;
        int _curDocIndex {0};
        Document _doc;
        bool _skipStep {false};
        unique_ptr<Impl> _impl;
    };

}

#endif
