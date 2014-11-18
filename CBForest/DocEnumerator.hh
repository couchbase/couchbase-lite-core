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

namespace forestdb {

    class DocEnumerator {
    public:
        struct Options {
            unsigned        skip;
            unsigned        limit;
            bool            descending;
            bool            inclusiveStart;
            bool            inclusiveEnd;
            bool            includeDeleted;
            bool            onlyConflicts;
            KeyStore::contentOptions  contentOptions;

            static const Options kDefault;
        };

        DocEnumerator(); // empty enumerator
        DocEnumerator(KeyStore,
                      slice startKey = slice::null,
                      slice endKey = slice::null,
                      const Options& options = Options::kDefault);
        DocEnumerator(KeyStore,
                      sequence start,
                      sequence end = UINT64_MAX,
                      const Options& options = Options::kDefault);
        DocEnumerator(KeyStore,
                      std::vector<std::string> docIDs,
                      const Options& options = Options::kDefault);
        DocEnumerator(DocEnumerator&& e); // move constructor
        ~DocEnumerator();

        DocEnumerator& operator=(DocEnumerator&& e); // move assignment

        bool next();
        bool seek(slice key);
        const Document& doc() const         {return *(Document*)_docP;}
        void close();

        // C++-like iterator API: for (auto e=db.enumerate(); e; ++e) {...}
        const DocEnumerator& operator++()   {next(); return *this;}
        operator const Document*() const    {return (const Document*)_docP;}
        const Document* operator->() const  {return (Document*)_docP;}

    protected:
        KeyStore _store;
        fdb_iterator *_iterator;
        alloc_slice _startKey;
        alloc_slice _endKey;
        Options _options;
        std::vector<std::string> _docIDs;
        int _curDocIndex;
        fdb_doc *_docP;
        bool _firstDescending; //HACK MB-12465

        friend class KeyStore;
        void setDocIDs(std::vector<std::string> docIDs);
        void restartFrom(slice firstKey);

        void freeDoc()                      {fdb_doc_free(_docP); _docP = NULL;}

    private:
        DocEnumerator(const DocEnumerator&); // no copying allowed
    };

}

#endif
