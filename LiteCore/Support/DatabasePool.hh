//
// DatabasePool.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Database.hh"
#include "fleece/RefCounted.hh"
#include <condition_variable>
#include <functional>
#include <mutex>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class FilePath;
    class BorrowedDatabase;

    /** A thread-safe pool of databases, for multi-threaded use. */
    class DatabasePool : public fleece::RefCounted {
      public:
        explicit DatabasePool(fleece::slice name, C4DatabaseConfig2 const&);

        /// Constructs a pool that will manage multiple instances of the given database.
        /// This database is now owned by the pool and shouldn't be used directly.
        /// If this database is opened read-only, then no writeable instances will be provided.
        explicit DatabasePool(C4Database*);

        /// The destructor waits until all borrowed databases have been returned.
        ~DatabasePool();

        /// The filesystem path of the database.
        FilePath databasePath() const;

        /// The maximum number of databases the pool will create, including one writeable one.
        /// Defaults to 5. Minimum value is 2 (otherwise why are you using a pool at all?)
        unsigned capacity() const noexcept;

        /// Sets the maximum number of databases the pool will create, including one writeable one.
        /// The default is 6. Minimum value is 2 (otherwise why are you using a pool at all?)
        void setCapacity(unsigned capacity);

        /// True if it's possible to get a writeable database.
        bool writeable() const noexcept { return _rwTotal > 0; }

        /// True if this pool manages the same file as this database.
        bool sameAs(C4Database* db) const noexcept;

        /// Registers a function that will be called just after a `C4Database` is opened,
        /// and can make connection-level changes.
        /// If `callNow` is true, the function will be called on the already-open databases
        /// (except for ones currently borrowed.)
        void onOpen(std::function<void(C4Database*)>, bool callNow = true) noexcept;

        /// The number of databases open, both borrowed and available.
        unsigned openCount() const noexcept;

        /// The number of databases currently borrowed. Ranges from 0 up to `capacity`.
        unsigned borrowedCount() const noexcept;

        /// Returns a `Retained` to a **read-only** database a client can use.
        /// When the `BorrowedDatabase` goes out of scope, the database is returned to the pool.
        /// @note  If all read-only databases are checked out, waits until one is returned.
        BorrowedDatabase borrow();

        /// Same as `borrow`, except returns an empty `BorrowedDatabase` instead of waiting.
        BorrowedDatabase tryBorrow();

        /// Returns a `Retained` to a **writeable** database a client can use.
        /// There is only one of these per pool, since LiteCore only supports one writer at a time.
        /// When the `BorrowedDatabase` goes out of scope, the database is returned to
        /// the pool.
        /// @note  If the writeable database is checcked out, waits until it's returned.
        /// @throws NotWriteable if there is no writeable database.
        BorrowedDatabase borrowWriteable();

        /// Same as `borrowWriteable`, except returns an empty `BorrowedDatabase` instead of waiting.
        /// @throws NotWriteable if there is no writeable database.
        BorrowedDatabase tryBorrowWriteable();

        /// Blocks until all borrowed databases have been returned, then closes them.
        /// (The destructor also does this.)
        void closeAll();

        /// Closes all databases the pool has opened that aren't currently in use.
        /// (The pool can still re-open more databases on demand, up to its capacity.)
        void closeUnused();

      private:
        friend class BorrowedDatabase;

        DatabasePool(DatabasePool&&)            = delete;
        DatabasePool& operator=(DatabasePool&&) = delete;
        unsigned      _borrowed_count() const;

        unsigned _open_count() const { return _roTotal + _rwTotal; }

        BorrowedDatabase             borrow(bool);
        BorrowedDatabase             borrowWriteable(bool);
        fleece::Retained<C4Database> newDB();
        void                         returnDatabase(C4Database*);
        void                         _closeUnused();

        std::string const                         _dbName;          // Name of database
        C4DatabaseConfig2                         _dbConfig;        // Database config
        fleece::alloc_slice                       _dbDir;           // Directory of database
        mutable std::mutex                        _mutex;           // Magic thread-safety voodoo
        mutable std::condition_variable           _cond;            // Magic thread-safety voodoo
        std::function<void(C4Database*)>          _initializer;     // Init fn called on each new database
        unsigned                                  _roCapacity = 5;  // Current capacity (of read-only dbs)
        unsigned                                  _roTotal    = 0;  // Number of read-only DBs I created
        unsigned                                  _rwCapacity = 1;  // Current capacity (of read-write dbs)
        unsigned                                  _rwTotal    = 0;  // Number of read-write DBs I created (0, 1)
        std::vector<fleece::Retained<C4Database>> _readonly;        // Stack of available RO DBs
        fleece::Retained<C4Database>              _readwrite;       // The available RW DB, or null
    };

    /** A wrapper around a C4Database "borrowed" from a DatabasePool.
        When it exits scope, the database is returned to the pool. */
    class BorrowedDatabase {
      public:
        BorrowedDatabase() : _pool(nullptr) {}

        BorrowedDatabase(BorrowedDatabase&& b) noexcept : _db(b._db), _pool(b._pool) { b._db = nullptr; }

        BorrowedDatabase& operator=(BorrowedDatabase&& b) noexcept;

        ~BorrowedDatabase() {
            if ( _db ) _pool->returnDatabase(_db);
        }

        explicit operator bool() const noexcept { return _db != nullptr; }

        C4Database* get() & noexcept LIFETIMEBOUND { return _db; }

        C4Database* operator->() noexcept LIFETIMEBOUND { return _db; }

        operator C4Database* C4NULLABLE () & noexcept LIFETIMEBOUND { return _db; }

        /// Returns the database to the pool.
        void reset();

      protected:
        friend class DatabasePool;

        BorrowedDatabase(C4Database* C4NULLABLE db, DatabasePool& pool) : _db(db), _pool(&pool) {}

      private:
        fleece::Retained<C4Database> _db;
        DatabasePool* C4NULLABLE     _pool;
    };

    class BorrowedCollection {
    public:
        BorrowedCollection(BorrowedDatabase&& db, C4CollectionSpec const& spec)
        :_bdb(std::move(db)), _collection(_bdb->getCollection(spec)) { }

        C4Collection* get() & noexcept LIFETIMEBOUND { return _collection; }

        C4Collection* operator->() noexcept LIFETIMEBOUND { return _collection; }

        operator C4Collection* C4NONNULL () & noexcept LIFETIMEBOUND { return _collection; }

    private:
        BorrowedDatabase _bdb;
        C4Collection* _collection;
    };

    class CollectionPool {
    public:
        CollectionPool(DatabasePool* pool, C4CollectionSpec const& spec)
        :_pool(pool), _scope(spec.scope), _name(spec.name) {}

        C4CollectionSpec spec() const {return {.name = _name, .scope = _scope};}

        BorrowedCollection borrow() {return {_pool->borrow(), spec()};}
        BorrowedCollection borrowWriteable() {return {_pool->borrowWriteable(), spec()};}

    private:
        fleece::Retained<DatabasePool> _pool;
        fleece::alloc_slice _scope, _name;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
