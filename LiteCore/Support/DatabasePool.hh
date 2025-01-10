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
#include "Error.hh"
#include "Logging.hh"
#include "fleece/RefCounted.hh"
#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class FilePath;
    class BorrowedDatabase;

    /** A concurrent pool of C4Database instances on a single file.
        A thread wanting to use the database can temporarily "borrow" an instance, wrapped in a
        `BorrowedDatabase` smart-pointer. The database is returned to the pool when the
        `BorrwedDatabase` object exits scope.

        The databases in the pool are opened read-only, except for one writeable instance.
        (Since SQLite allows concurrent readers but only a single writer, this ensures that no two
        borrowed databases will block each other.) Therefore, if there is a possibility you need
        to write to the database, you must call `borrowWriteable` or else you'll get database-
        locked errors. (The writeable database is _only_ checked out by borrowWriteable, to improve
        availability.)

        If you try to borrow but all matching databases are checked out, the method blocks until
        one is returned; but after waiting ten seconds it will throw a `Busy` exception.
        If you don't want to block, call one of the `tryBorrow` methods, which return an empty/null
        `BorrowedDatabase` instead of blocking.

        @warning Watch out for nested borrows! If you borrow a database, then call another function
        that also borrows from the same pool, you've now borrowed two instances, which is wasteful.
        If you borrow the _writeable_ database twice, the second/nested call will deadlock until
        it times out and throws a `Busy` exception. */

    class DatabasePool
        : public fleece::RefCounted
        , public Logging {
      public:
        /// Constructs a pool that will manage multiple instances of the given database file.
        /// If the `kC4DB_ReadOnly` flag is set, no writeable instances will be provided.
        /// @note  This method does not attempt to open a database. If the database can't be opened,
        ///         you won't get an exception until you try to borrow it.
        explicit DatabasePool(fleece::slice name, C4DatabaseConfig2 const&);

        /// Constructs a pool that will manage multiple instances of the given database file.
        /// If this database was opened read-only, then no writeable instances will be provided.
        /// @warning  The C4Database is now owned by the pool and shouldn't be used directly.
        explicit DatabasePool(C4Database*);

        /// Closes all databases, waiting until all borrowed ones have been returned.
        /// No more databases can be borrowed after this method begins.
        void close();

        /// Closes all databases the pool has opened that aren't currently in use.
        /// (The pool can still re-open more databases on demand, up to its capacity.)
        void closeUnused();

        /// The database configuration.
        C4DatabaseConfig2 const& getConfiguration() const { return _dbConfig; }

        /// The filesystem path of the database.
        FilePath databasePath() const;

        /// True if it's possible to get a writeable database.
        bool writeable() const noexcept { return _readWrite.capacity > 0; }

        /// The maximum number of databases the pool will create.
        /// Defaults to 5, or 4 if not writeable.
        unsigned capacity() const noexcept;

        /// Sets the maximum number of databases the pool will create, including any writeable one.
        /// Minimum value is 2 (otherwise why are you using a pool at all?)
        void setCapacity(unsigned capacity);

        /// True if this pool manages the same file as this database.
        bool sameAs(C4Database* db) const noexcept;

        /// Registers a function that will be called just after a new `C4Database` is opened.
        /// Or if `nullptr` is passed, it clears any already-set function.
        /// The function may make connection-level changes.
        /// If `callNow` is true, the function will be immediately be called on the databases
        /// already in the pool, _except_ for ones currently borrowed.
        void onOpen(std::function<void(C4Database*)>, bool callNow = true);

        /// The number of databases open, both borrowed and available.
        unsigned openCount() const noexcept;

        /// The number of databases currently borrowed. Ranges from 0 up to `capacity`.
        unsigned borrowedCount() const noexcept;

        /// Returns a smart-pointer to a **read-only** C4Database a client can use.
        /// When the `BorrowedDatabase` goes out of scope, the database is returned to the pool.
        /// You must not use the C4Database reference after that!
        /// @note  If all read-only databases are checked out, waits until one is returned.
        /// @throws litecore::error `NotOpen` if `close` has been called.
        /// @throws litecore::error `Timeout` after waiting ten seonds.
        BorrowedDatabase borrow();

        /// Same as `borrow`, except that if no databases are available to borrow it returns an
        /// empty `BorrowedDatabase` instead of waiting.
        /// You must check the returned object to see if the C4Database it's holding is `nullptr`.
        BorrowedDatabase tryBorrow();

        /// Returns a smart-pointer to a **writeable** database a client can use.
        /// (There is only one of these per pool, since LiteCore only supports one writer at a time.)
        /// When the `BorrowedDatabase` goes out of scope, the database is returned to the pool.
        /// You must not use the C4Database reference after that!
        /// @note  If the writeable database is checcked out, waits until it's returned.
        /// @throws litecore::error `NotWriteable` if `writeable()` is false.
        /// @throws litecore::error `NotOpen` if `close` has been called.
        /// @throws litecore::error `Timeout` after waiting ten seonds.
        BorrowedDatabase borrowWriteable();

        /// Same as `borrowWriteable`, except that if no databases are available to borrow it returns an
        /// empty `BorrowedDatabase` instead of waiting.
        /// You must check the returned object to see if the C4Database it's holding is `nullptr`.
        /// @throws litecore::error `NotWriteable` if `writeable()` is false.
        /// @throws litecore::error `NotOpen` if `close` has been called.
        BorrowedDatabase tryBorrowWriteable();

        class Transaction;

        /// Creates a Transaction. Equivalent to `DatabasePool::Transaction(*pool)`.
        inline Transaction transaction();

        /// Calls the function/lambda in a transaction, passing it the `C4Database*`.
        /// The transaction automatically commits after `fn` returns, or aborts if it throws.
        inline auto inTransaction(auto fn);

      protected:
        ~DatabasePool() override;
        std::string loggingIdentifier() const override;

      private:
        friend class BorrowedDatabase;

        static constexpr size_t kMaxCapacity = 8;

        /** A cache of available db instances with the same access, either read-only or read-write. */
        struct Cache {
            Cache(C4DatabaseFlags, unsigned capacity);

            struct Entry {
                Retained<C4Database> db;               ///< Database (may be nullptr)
                unsigned             borrowCount = 0;  ///< Number of borrows
                std::thread::id      borrower;         ///< Thread that borrowed it, if any
            };

            C4DatabaseFlags const           flags;          ///< Flags for opening dbs
            unsigned                        capacity;       ///< Max number of open dbs
            unsigned                        created   = 0;  ///< Number of open dbs
            unsigned                        available = 0;  ///< Number of un-borrowed open dbs
            std::array<Entry, kMaxCapacity> entries;        ///< Tracks each db and its borrowers

            bool writeable() const noexcept { return (flags & kC4DB_ReadOnly) == 0; }

            unsigned borrowedCount() const noexcept { return created - available; }
        };

        DatabasePool(DatabasePool&&)                = delete;
        DatabasePool&     operator=(DatabasePool&&) = delete;
        BorrowedDatabase  borrow(Cache& cache, bool orWait);
        [[noreturn]] void borrowFailed(Cache&);

        fleece::Retained<C4Database> newDB(Cache&);
        void                         closeDB(Retained<C4Database>) noexcept;
        void                         returnDatabase(fleece::Retained<C4Database>);
        void                         _closeUnused(Cache&);
        void                         _closeAll(Cache&);

        std::string const                _dbName;          // Name of database
        C4DatabaseConfig2                _dbConfig;        // Database config
        fleece::alloc_slice              _dbDir;           // Parent directory of database
        mutable std::mutex               _mutex;           // Thread-safety
        mutable std::condition_variable  _cond;            // Used for waiting for a db
        std::function<void(C4Database*)> _initializer;     // Init fn called on each new database
        Cache                            _readOnly;        // Manages read-only databases
        Cache                            _readWrite;       // Manages writeable databases
        int                              _dbTag  = -1;     // C4DatabaseTag
        bool                             _closed = false;  // Set by `close`
    };

    /** An RAII wrapper around a C4Database "borrowed" from a DatabasePool.
        When it exits scope, the database is returned to the pool.
        @note A `BorrowedDatabase`s lifetime should be kept short, and limited to a single thread. */
    class BorrowedDatabase {
      public:
        /// Constructs an empty BorrowedDatabase. (Use `operator bool` to test for emptiness.)
        BorrowedDatabase() noexcept = default;

        /// Borrows a (read-only) database from a pool. Equivalent to calling `pool.borrow()`.
        explicit BorrowedDatabase(DatabasePool* pool) : BorrowedDatabase(pool->borrow()) {}

        /// "Borrows" a database without a pool -- simply retains the db and acts as a smart pointer
        /// to it. This allows you to use `BorrowedDatabase` with or without a `DatabasePool`.
        explicit BorrowedDatabase(C4Database* db) noexcept : _db(db) {}

        BorrowedDatabase(BorrowedDatabase&& b) noexcept = default;

        BorrowedDatabase& operator=(BorrowedDatabase&& b) noexcept;

        ~BorrowedDatabase() { _return(); }

        /// Checks whether I am non-empty.
        /// @note It's illegal to dereference an empty instance. The only way to create such
        ///       an instance is with the default constructor, or `DatabasePool::tryBorrow`.
        explicit operator bool() const noexcept { return _db != nullptr; }

        C4Database* get() const noexcept LIFETIMEBOUND {
            DebugAssert(_db);
            return _db;
        }

        C4Database* operator->() const noexcept LIFETIMEBOUND { return get(); }

        operator C4Database* C4NONNULL() const noexcept LIFETIMEBOUND { return get(); }

        /// Returns the database to the pool, leaving me empty.
        void reset();

      protected:
        friend class DatabasePool;
        BorrowedDatabase(BorrowedDatabase const&)            = delete;
        BorrowedDatabase& operator=(BorrowedDatabase const&) = delete;

        // used by DatabasePool::borrow methods
        BorrowedDatabase(fleece::Retained<C4Database> db, DatabasePool* pool) : _db(std::move(db)), _pool(pool) {}

      private:
        void _return();

        fleece::Retained<C4Database>   _db;
        fleece::Retained<DatabasePool> _pool;
    };

    /** An RAII wrapper around a collection of a database "borrowed" from a `DatabasePool`.
        When it exits scope, its database is returned to the pool. */
    class BorrowedCollection {
      public:
        /// Constructs an empty BorrowedCollection. (Use `operator bool` to test for emptiness.)
        BorrowedCollection() noexcept;

        /// Constructor.
        /// @throws `error::NotFound` if there is a database but no such collection in it.
        BorrowedCollection(BorrowedDatabase&& bdb, C4CollectionSpec const& spec);

        BorrowedCollection(BorrowedCollection&& b) noexcept            = default;
        BorrowedCollection& operator=(BorrowedCollection&& b) noexcept = default;
        ~BorrowedCollection();

        /// Checks whether I am non-empty, i.e. I have a a collection.
        explicit operator bool() const noexcept { return _collection != nullptr; }

        C4Collection* get() const noexcept LIFETIMEBOUND {
            DebugAssert(_collection);
            return _collection;
        }

        C4Collection* operator->() const noexcept LIFETIMEBOUND { return get(); }

        operator C4Collection* C4NONNULL() const noexcept LIFETIMEBOUND { return get(); }

      private:
        BorrowedDatabase       _bdb;
        Retained<C4Collection> _collection{};
    };

    /** Subclass of C4Database::Transaction : a transaction on a borrowed (writeable) database.
        Remember to call `commit`!
        @note  Using this avoids the footgun `C4Database::Transaction t(pool.borrowWriteable());`
               which unintentionally returns the database to the pool immediately! */
    class DatabasePool::Transaction
        : private BorrowedDatabase
        , public C4Database::Transaction {
      public:
        explicit Transaction(DatabasePool& pool)
            : BorrowedDatabase(pool.borrowWriteable()), C4Database::Transaction(db()) {}

        C4Database* db() const noexcept LIFETIMEBOUND { return get(); }
    };

    inline DatabasePool::Transaction DatabasePool::transaction() { return Transaction(*this); }

    inline auto DatabasePool::inTransaction(auto fn) {
        Transaction txn(*this);
        fn(txn.db());
        if ( txn.isActive() ) txn.commit();
    }


}  // namespace litecore

C4_ASSUME_NONNULL_END
