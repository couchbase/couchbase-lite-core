//
//  C4DatabaseInternal.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "CASRevisionStore.hh"

namespace c4Internal {
    class C4DocumentInternal;
}
namespace litecore {
    class FilePath;
}


// c4Database must be in the global namespace because it's forward-declared in the C API.

struct c4Database : public RefCounted<c4Database> {

    static c4Database* newDatabase(string pathStr,
                                   C4DatabaseConfig config);

    static DataFile* newDataFile(string path,
                                 const C4DatabaseConfig &config,
                                 bool isMainDB);

    DataFile* db()                                      {return _db.get();}

    const C4DatabaseConfig config;

    bool mustUseVersioning(C4DocumentVersioning, C4Error*) noexcept;

    Transaction& transaction() {
        Assert(_transaction);
        return *_transaction;
    }

    // Transaction methods below acquire _transactionMutex. Do not call them if
    // _mutex is already locked, or deadlock may occur!
    void beginTransaction();
    bool inTransaction() noexcept;
    bool mustBeInTransaction(C4Error *outError) noexcept;
    bool mustNotBeInTransaction(C4Error *outError) noexcept;
    bool endTransaction(bool commit);

    KeyStore& defaultKeyStore()                         {return _db->defaultKeyStore();}
    KeyStore& getKeyStore(const string &name) const     {return _db->getKeyStore(name);}

    virtual C4DocumentInternal* newDocumentInstance(C4Slice docID) =0;
    virtual C4DocumentInternal* newDocumentInstance(const Document&) =0;
    virtual bool readDocMeta(const Document&,
                             C4DocumentFlags*,
                             alloc_slice *revID =nullptr,
                             slice *docType =nullptr) =0;

    static bool rekey(DataFile* database, const C4EncryptionKey *newKey, C4Error*) noexcept;

#if C4DB_THREADSAFE
    // Mutex for synchronizing DataFile calls. Non-recursive!
    mutex _mutex;
#endif

protected:
    c4Database(string path,
               const C4DatabaseConfig &config);
    virtual ~c4Database() { Assert(_transactionLevel == 0); }

    static FilePath findOrCreateBundle(const string &path, C4DatabaseConfig &config);

private:
    unique_ptr<DataFile>   _db;                    // Underlying DataFile
    Transaction*                _transaction {nullptr};    // Current Transaction, or null
    int                         _transactionLevel {0};  // Nesting level of transaction
#if C4DB_THREADSAFE
    // Recursive mutex for accessing _transaction and _transactionLevel.
    // Must be acquired BEFORE _mutex, or deadlock may occur!
    recursive_mutex        _transactionMutex;
#endif
};


namespace c4Internal {

    // Subclass for old (rev-tree) schema
    class c4DatabaseV1 : public c4Database {
    public:
        c4DatabaseV1(string path, const C4DatabaseConfig &config)
        :c4Database(path, config)
        { }
        C4DocumentInternal* newDocumentInstance(C4Slice docID) override;
        C4DocumentInternal* newDocumentInstance(const Document&) override;
        bool readDocMeta(const Document&,
                         C4DocumentFlags*,
                         alloc_slice *revID =nullptr,
                         slice *docType =nullptr) override;
    };


    // Subclass for new (version-vector) schema
    class c4DatabaseV2 : public c4Database {
    public:
        c4DatabaseV2(string path, const C4DatabaseConfig &config)
        :c4Database(path, config)
        { }

        C4DocumentInternal* newDocumentInstance(C4Slice docID) override;
        C4DocumentInternal* newDocumentInstance(const Document&) override;

        CASRevisionStore& revisionStore();

        bool readDocMeta(const Document&,
                         C4DocumentFlags*,
                         alloc_slice *revID =nullptr,
                         slice *docType =nullptr) override;

    private:
        unique_ptr<CASRevisionStore> _revisionStore;
    };

}


#if C4DB_THREADSAFE
#define WITH_LOCK(db) lock_guard<mutex> _lock((db)->_mutex)
#else
#define WITH_LOCK(db) do { } while (0)  // no-op
#endif

