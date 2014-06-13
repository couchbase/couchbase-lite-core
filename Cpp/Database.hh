//
//  Database.h
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Database__
#define __CBForest__Database__
#include "forestdb.h"
#include "slice.h"
#include <vector>

#ifdef check
#undef check
#endif

namespace forestdb {

    class Document;
    class DocEnumerator;
    class Transaction;

    typedef fdb_seqnum_t sequence;

    // Most API calls can throw this
    struct error {
        fdb_status status;
        error (fdb_status s)        :status(s) {}
    };


    /** Base class of Database and Transaction: defines read-only API. */
    class DatabaseGetters {
    public:
        typedef fdb_open_flags openFlags;
        typedef fdb_config config;
        typedef fdb_info info;

        std::string filename() const;
        info getInfo() const;

        // Keys/values:

        enum contentOptions {
            kDefaultContent = 0,
            kMetaOnly = 0x01
        };

        Document get(slice key, contentOptions = kDefaultContent) const;
        Document get(sequence, contentOptions = kDefaultContent) const;
        bool read(Document&, contentOptions = kDefaultContent) const; // key must already be set

        Document getByOffset(uint64_t offset);

        // Enumeration:

        struct enumerationOptions {
            unsigned        skip;
            unsigned        limit;
            bool            descending;
            bool            inclusiveEnd;
            bool            includeDeleted;
            bool            onlyConflicts;
            contentOptions  contentOptions;

            static const enumerationOptions kDefault;
        };

        DocEnumerator enumerate(slice startKey = slice::null,
                                slice endKey = slice::null,
                                const enumerationOptions* = NULL);
        DocEnumerator enumerate(sequence start,
                                sequence end = UINT64_MAX,
                                const enumerationOptions* = NULL);
        DocEnumerator enumerate(std::vector<std::string> docIDs,
                                const enumerationOptions* = NULL);

    protected:
        DatabaseGetters();
        virtual ~DatabaseGetters() {}

        fdb_handle* _handle;
    };


    /** ForestDB database. Inherits read-only access from DatabaseGetters; to write to the
        database, create a Transaction from it and use that. */
    class Database : public DatabaseGetters {
    public:
        static config defaultConfig()           {return fdb_get_default_config();}

        Database(std::string path, openFlags, const config&);
        Database(Database* original, sequence snapshotSequence);
        virtual ~Database();

        bool isReadOnly() const;

    private:
        class File;
        friend class Transaction;
        fdb_handle* beginTransaction(Transaction*,sequence&);
        void endTransaction(fdb_handle* handle);

        File* _file;
        openFlags _openFlags;
        config _config;
    };


    /** Grants exclusive write access to a Database and provides APIs to write documents.
        The transaction is committed when the object exits scope.
        Only one Transaction object can be created on a database file at a time.
        Not just per Database object; per database _file_. */
    class Transaction : public DatabaseGetters {
    public:
        Transaction(Database*);
        ~Transaction();

        /** Tells the Transaction that it should rollback, not commit, when exiting scope. */
        void abort() {_state = -1;}
        
        void deleteDatabase();
        void erase();

        void rollbackTo(sequence);
        void compact();

        /** Records a commit before the transaction exits scope. Not normally needed. */
        void commit();

        sequence set(slice key, slice meta, slice value);
        sequence set(slice key, slice value);
        void write(Document&);

        void del(slice key);
        void del(sequence);
        void del(Document&);

    private:
        void check(fdb_status status);

        Database& _db;
        sequence _startSequence;
        int _state;
    };


    /** Stores a document's key, metadata and body as slices. Memory is owned by the object and
        will be freed when it destructs. Setters copy, getters don't. */
    class Document {
    public:
        Document();
        Document(slice key);
        ~Document();

        slice key() const   {return slice(_doc.key, _doc.keylen);}
        slice meta() const  {return slice(_doc.meta, _doc.metalen);}
        slice body() const  {return slice(_doc.body, _doc.bodylen);}

        void setKey(slice key);
        void setMeta(slice meta);
        void setBody(slice body);

        void clearMetaAndBody();

        sequence sequence() const   {return _doc.seqnum;}
        uint64_t offset() const     {return _doc.offset;}
        size_t sizeOnDisk() const   {return _doc.size_ondisk;}
        bool deleted() const        {return _doc.deleted;}
        bool exists() const         {return _doc.offset > 0;}

        typedef DocEnumerator enumerator;

    private:
        friend class DatabaseGetters;
        friend class Transaction;
        operator fdb_doc*() {return &_doc;}

        fdb_doc _doc;
    };
    

    class DocEnumerator {
    public:
        DocEnumerator(); // empty enumerator
        DocEnumerator(DocEnumerator&& e); // move constructor
        ~DocEnumerator();

        bool next();
        bool seek(slice key);
        const Document& doc() const         {return *(Document*)_docP;}
        void close();

        DocEnumerator& operator=(DocEnumerator&& e);

        // C++-like iterator API: for (auto e=db.enumerate(); e; ++e) {...}
        const DocEnumerator& operator++()   {next(); return *this;}
        operator const Document*() const    {return (const Document*)_docP;}
        const Document* operator->() const  {return (Document*)_docP;}

    protected:
        fdb_iterator *_iterator;
        std::vector<std::string> _docIDs;
        std::vector<std::string>::const_iterator _curDocID;
        Database::contentOptions _options;
        fdb_doc *_docP;

        friend class DatabaseGetters;
        DocEnumerator(fdb_iterator*, const Database::enumerationOptions*);
        DocEnumerator(fdb_iterator*, std::vector<std::string> docIDs,
                      const Database::enumerationOptions*);
        void setDocIDs(std::vector<std::string> docIDs);
    private:
        DocEnumerator(const DocEnumerator& e); // no copying allowed
    };

}

#endif /* defined(__CBForest__Database__) */
