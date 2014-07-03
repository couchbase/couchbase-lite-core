//
//  Database.hh
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Database__
#define __CBForest__Database__
#include "Error.hh"
#include "forestdb.h"
#include "slice.hh"
#include <vector>

#ifdef check
#undef check
#endif

namespace forestdb {

    class Document;
    class DocEnumerator;
    class Transaction;

    typedef fdb_seqnum_t sequence;


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

        Document getByOffset(uint64_t offset, sequence);

    protected:
        DatabaseGetters();
        virtual ~DatabaseGetters() {}

        fdb_handle* _handle;

    private:
        DatabaseGetters(const DatabaseGetters&); // forbidden
        friend class DocEnumerator;
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

        void setLogCallback(fdb_log_callback callback, void* ctx_data) {
            fdb_set_log_callback(_handle, callback, ctx_data);
        }

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

        Transaction(const Transaction&); // forbidden

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
        Document(const Document&); // this copies the key/meta/value
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

        sequence sequence() const   {return _doc.seqnum;}
        uint64_t offset() const     {return _doc.offset;}
        size_t sizeOnDisk() const   {return _doc.size_ondisk;}
        bool deleted() const        {return _doc.deleted;}
        bool exists() const         {return _doc.size_ondisk > 0 || _doc.offset > 0;}

        typedef DocEnumerator enumerator;

    private:
        friend class DatabaseGetters;
        friend class Transaction;
        operator fdb_doc*() {return &_doc;}

        Document& operator= (const Document&);

        fdb_doc _doc;
    };
    
}

#endif /* defined(__CBForest__Database__) */
