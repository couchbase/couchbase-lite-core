//
//  DocEnumerator.hh
//  CBForest
//
//  Created by Jens Alfke on 6/18/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Database.hh"

namespace forestdb {

    class DocEnumerator {
    public:
        struct Options {
            unsigned        skip;
            unsigned        limit;
//          bool            descending;     //TODO: Unimplemented in forestdb (MB-10961)
            bool            inclusiveEnd;
            bool            includeDeleted;
            bool            onlyConflicts;
            Database::contentOptions  contentOptions;

            static const Options kDefault;
        };

        DocEnumerator(); // empty enumerator
        DocEnumerator(DatabaseGetters* db,
                      slice startKey = slice::null,
                      slice endKey = slice::null,
                      const Options& options = Options::kDefault);
        DocEnumerator(DatabaseGetters* db,
                      sequence start,
                      sequence end = UINT64_MAX,
                      const Options& options = Options::kDefault);
        DocEnumerator(DatabaseGetters* db,
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
        DatabaseGetters* _db;
        fdb_iterator *_iterator;
        alloc_slice _endKey;
        Options _options;
        std::vector<std::string> _docIDs;
        int _curDocIndex;
        fdb_doc *_docP;

        friend class DatabaseGetters;
        void setDocIDs(std::vector<std::string> docIDs);
        void restartFrom(slice startKey, slice endKey = slice::null);

        void freeDoc()                      {fdb_doc_free(_docP); _docP = NULL;}

    private:
        DocEnumerator(const DocEnumerator&); // no copying allowed
    };

}
