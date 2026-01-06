//
// DatabasePool.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "DatabasePool.hh"
#include "DatabaseImpl.hh"  // for asInternal, dataFile
#include "Delimiter.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "c4Collection.hh"
#include "c4ExceptionUtils.hh"
#include <sstream>
#include <chrono>

namespace litecore {
    using namespace std;
    using namespace fleece;


    /// The default number of read-only C4Databases in a pool.
    static constexpr size_t kDefaultReadOnlyCapacity = 4;

/// How long a thread will wait wait to borrow a C4Database, before throwing error::Busy.
/// The exception is intended for detecting & breaking deadlocks, and should never happen in real use.
/// 10 seconds seemed a reasonable value, but in some cases like replicating huge numbers of
/// collections there can be enough contention that the timeout actually expires,
/// and we don't want to turn a performance problem into a failure. [CBSE-21760]
/// Thus we set it to a ridiculously high value in release builds.
#if DEBUG
    static constexpr auto kTimeout = 10s;
#else
    static constexpr auto kTimeout = 5min;
#endif


    static string nameOf(C4Database* db) { return asInternal(db)->dataFile()->loggingName(); }

    DatabasePool::DatabasePool(slice name, C4DatabaseConfig2 const& config)
        : Logging(DBLog)
        , _dbName(name)
        , _dbConfig(config)
        , _dbDir(_dbConfig.parentDirectory)
        , _readOnly(config.flags | kC4DB_ReadOnly, kDefaultReadOnlyCapacity)
        , _readWrite(config.flags & ~kC4DB_ReadOnly, (_dbConfig.flags & kC4DB_ReadOnly) ? 0 : 1) {
        _dbConfig.parentDirectory = _dbDir;
    }

    DatabasePool::DatabasePool(Ref<C4Database>&& main) : DatabasePool(main->getName(), main->getConfiguration()) {
        logInfo("initial database is %s", nameOf(main).c_str());
        _dbTag              = _c4db_getDatabaseTag(main);
        Cache& cache        = writeable() ? _readWrite : _readOnly;
        cache.entries[0].db = main;
        cache.created++;
        cache.available++;
    }

    DatabasePool::~DatabasePool() { close(); }

    string DatabasePool::loggingIdentifier() const { return _dbName; }

    void DatabasePool::close() {
        unique_lock lock(_mutex);
        if ( !_closed ) {
            logInfo("Closing pool...");
            _closed = true;
            _cond.notify_all();  // unblocks borrow() calls so they can throw NotOpen
        }
        _closeUnused(_readOnly);
        _closeUnused(_readWrite);

        if ( auto remaining = _readOnly.created + _readWrite.created ) {
            logInfo("Waiting for %u borrowed dbs to be returned...", remaining);
            auto timeout = std::chrono::system_clock::now() + kTimeout;
            bool ok      = _cond.wait_until(lock, timeout, [&] { return _readOnly.created + _readWrite.created == 0; });
            if ( !ok ) error::_throw(error::Busy, "Timed out closing DatabasePool");
            Assert(_readOnly.created + _readWrite.created == 0);
        }
        logInfo("...all databases closed!");
    }

    FilePath DatabasePool::databasePath() const { return FilePath{_dbDir, _dbName + kC4DatabaseFilenameExtension}; }

    unsigned DatabasePool::capacity() const noexcept {
        unique_lock lock(_mutex);
        return _readOnly.capacity + _readWrite.capacity;
    }

    void DatabasePool::setCapacity(unsigned newCapacity) {
        Assert(newCapacity <= kMaxCapacity);
        unique_lock lock(_mutex);
        if ( _closed ) error::_throw(error::NotOpen, "DatabasePool is closed");
        Assert(newCapacity >= 1 + _readWrite.capacity, "capacity too small");
        _readOnly.capacity = newCapacity - _readWrite.capacity;
        Assert(_readOnly.capacity >= 1);

        // Toss out any excess available RO databases:
        for ( auto& entry : _readOnly.entries ) {
            if ( _readOnly.created > _readOnly.capacity && entry.db && entry.borrowCount == 0 ) {
                closeDB(std::move(entry.db).asRef());
                --_readOnly.available;
                --_readOnly.created;
            }
        }
    }

    bool DatabasePool::sameAs(C4Database* db) const noexcept {
        return db->getName() == _dbName && db->getConfiguration().parentDirectory == _dbConfig.parentDirectory;
    }

    void DatabasePool::onOpen(std::function<void(C4Database*)> init, bool callNow) {
        unique_lock lock(_mutex);
        _initializer = std::move(init);
        if ( callNow && _initializer ) {
            for ( auto& entry : _readOnly.entries )
                if ( entry.db ) _initializer(entry.db);
            for ( auto& entry : _readWrite.entries )
                if ( entry.db ) _initializer(entry.db);
        }
    }

    unsigned DatabasePool::openCount() const noexcept {
        unique_lock lock(_mutex);
        return _readOnly.created + _readWrite.created;
    }

    unsigned DatabasePool::borrowedCount() const noexcept {
        unique_lock lock(_mutex);
        return _readOnly.borrowedCount() + _readWrite.borrowedCount();
    }

    void DatabasePool::closeUnused() {
        unique_lock lock(_mutex);
        _closeUnused(_readOnly);
        _closeUnused(_readWrite);
    }

    void DatabasePool::_closeUnused(Cache& cache) {
        for ( auto& entry : cache.entries ) {
            if ( entry.borrowCount == 0 && entry.db ) {
                closeDB(std::move(entry.db).asRef());
                --cache.created;
                --cache.available;
            }
        }
    }

    // Allocates and opens a new C4Database instance.
    Ref<C4Database> DatabasePool::newDB(Cache& cache) {
        auto config        = _dbConfig;
        config.flags       = cache.flags;
        Ref<C4Database> db = C4Database::openNamed(_dbName, config).asRef();
        if ( _dbTag >= 0 ) _c4db_setDatabaseTag(db, C4DatabaseTag(_dbTag));
        if ( _initializer ) {
            try {
                _initializer(db);
            } catch ( ... ) {
                closeDB(db);
                throw;
            }
        }
        logInfo("created %s", nameOf(db).c_str());
        return db;
    }

    void DatabasePool::closeDB(Ref<C4Database> db) noexcept {
        logInfo("closing %s", nameOf(db).c_str());
        try {
            db->close();
        }
        catchAndWarn();
    }

    BorrowedDatabase DatabasePool::borrow(Cache& cache, bool or_wait) {
        auto tid = std::this_thread::get_id();

        unique_lock lock(_mutex);
        while ( true ) {
            if ( _closed ) error::_throw(error::NotOpen, "DatabasePool is closed");

            auto borrow = [&](Cache::Entry& entry) -> BorrowedDatabase {
                DebugAssert(entry.db);
                DebugAssert(entry.borrower == tid);
                ++entry.borrowCount;
                return BorrowedDatabase(entry.db, this);
            };

            if ( !cache.writeable() && cache.borrowedCount() > 0 ) {
                // A thread can borrow the same read-only database multiple times:
                for ( auto& entry : cache.entries ) {
                    if ( entry.borrower == tid ) {
                        DebugAssert(entry.borrowCount > 0);
                        return borrow(entry);
                    }
                }
            }
            if ( cache.available > 0 ) {
                // Look for an available database:
                for ( auto& entry : cache.entries ) {
                    if ( entry.db && entry.borrowCount == 0 ) {
                        DebugAssert(entry.borrower == thread::id{});
                        entry.borrower = tid;
                        --cache.available;
                        return borrow(entry);
                    }
                }
            }
            if ( cache.borrowedCount() < cache.capacity ) {
                // Open a new C4Database if cache is not yet at capacity:
                for ( auto& entry : cache.entries ) {
                    if ( entry.db == nullptr ) {
                        DebugAssert(entry.borrowCount == 0);
                        entry.db = newDB(cache);
                        ++cache.created;
                        entry.borrower = tid;
                        return borrow(entry);
                    }
                }
            }

            // Couldn't borrow a database:
            if ( cache.capacity == 0 ) {
                Assert(cache.writeable());
                error::_throw(error::NotWriteable, "Database is read-only");
            }

            if ( !or_wait ) return BorrowedDatabase(nullptr, this);

            // Nothing available, so wait and retry
            auto timeout = std::chrono::system_clock::now() + kTimeout;
            if ( _cond.wait_until(lock, timeout) == std::cv_status::timeout ) borrowFailed(cache);
        }
    }

    __cold void DatabasePool::borrowFailed(Cache& cache) {
        // Try to identify the source of the deadlock:
        stringstream out;
        out << "Thread " << std::this_thread::get_id() << " timed out waiting on DatabasePool::borrow ["
            << (&cache == &_readWrite ? "writeable" : "read-only") << "]. Borrowers are ";
        delimiter comma;
        for ( auto& entry : cache.entries ) {
            if ( entry.borrowCount ) out << comma << entry.borrower;
        }
        error::_throw(error::Busy, "%s", out.str().c_str());
    }

    BorrowedDatabase DatabasePool::borrow() { return borrow(_readOnly, true); }

    BorrowedDatabase DatabasePool::tryBorrow() { return borrow(_readOnly, false); }

    BorrowedDatabase DatabasePool::borrowWriteable() { return borrow(_readWrite, true); }

    BorrowedDatabase DatabasePool::tryBorrowWriteable() { return borrow(_readWrite, false); }

    // Called by BorrowedDatabase's destructor and its reset method.
    void DatabasePool::returnDatabase(Ref<C4Database> db) {
        DebugAssert(db);
        unique_lock lock(_mutex);

        Cache& cache = (db->getConfiguration().flags & kC4DB_ReadOnly) ? _readOnly : _readWrite;
        Assert(cache.borrowedCount() > 0);

        for ( auto& entry : cache.entries ) {
            if ( entry.db == db ) {
                auto tid = this_thread::get_id();
                if ( entry.borrower != tid )
                    Warn("DatabasePool::returnDatabase: Calling thread is not the same that borrowed db");
                Assert(entry.borrowCount > 0);
                Assert(entry.borrowCount > 1 || !db->isInTransaction(), "Returning db while in transaction");
                if ( --entry.borrowCount == 0 ) {
                    entry.borrower = {};
                    if ( cache.created > cache.capacity || _closed ) {
                        // Toss out a DB if capacity was lowered after it was checked out, or I'm closed:
                        closeDB(std::move(db));
                        entry.db = nullptr;
                        --cache.created;
                    } else {
                        ++cache.available;
                    }
                }

                _cond.notify_all();  // wake up waiting `borrow` and `close` calls
                return;
            }
        }
        error::_throw(error::AssertionFailed, "DatabasePool::returnDatabase: db does not belong to pool");
    }

#pragma mark - CACHE:

    DatabasePool::Cache::Cache(C4DatabaseFlags flags_, unsigned capacity_)
        : flags(flags_ & ~kC4DB_Create), capacity(capacity_) {
        Assert(capacity <= kMaxCapacity);
    }

    // Retained<C4Database> DatabasePool::Cache::pop() {
    //     Retained<C4Database> db;
    //     if ( !available.empty() ) {
    //         db = std::move(available.back());
    //         available.pop_back();
    //     }
    //     return db;
    // }


#pragma mark - BORROWED DATABASE:

    BorrowedDatabase& BorrowedDatabase::operator=(BorrowedDatabase&& b) noexcept {
        _return();
        _db   = std::move(b._db);
        _pool = std::move(b._pool);
        return *this;
    }

    void BorrowedDatabase::reset() {
        _return();
        _db   = nullptr;
        _pool = nullptr;
    }

    void BorrowedDatabase::_return() {
        if ( _db && _pool ) _pool->returnDatabase(std::move(_db).asRef());
    }

    BorrowedCollection::BorrowedCollection(BorrowedDatabase&& bdb, C4CollectionSpec const& spec)
        : _bdb(std::move(bdb)), _collection(_bdb ? _bdb->getCollection(spec) : nullptr) {
        if ( _bdb && !_collection ) error::_throw(error::NotFound, "no such collection");
    }

    BorrowedCollection::BorrowedCollection() noexcept = default;
    BorrowedCollection::~BorrowedCollection()         = default;

}  // namespace litecore
