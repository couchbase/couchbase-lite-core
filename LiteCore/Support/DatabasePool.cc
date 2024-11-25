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
#include "FilePath.hh"
#include "Logging.hh"
#include "c4ExceptionUtils.hh"
#include <unistd.h>
#include <sys/fcntl.h>

namespace litecore {
    using namespace std;
    using namespace fleece;

    static constexpr size_t kDefaultReadOnlyCapacity = 4;

    static constexpr auto kTimeout = 10s;

    static string nameOf(C4Database* db) { return asInternal(db)->dataFile()->loggingName(); }

    DatabasePool::DatabasePool(slice name, C4DatabaseConfig2 const& config)
        : Logging(DBLog)
        , _dbName(name)
        , _dbConfig(config)
        , _dbDir(_dbConfig.parentDirectory)
        , _readOnly{.flags = (config.flags | kC4DB_ReadOnly) & ~kC4DB_Create}
        , _readWrite{.flags = (config.flags & ~kC4DB_ReadOnly) & ~kC4DB_Create} {
        _dbConfig.parentDirectory = _dbDir;
        _readOnly.capacity        = kDefaultReadOnlyCapacity;
        _readWrite.capacity       = (_dbConfig.flags & kC4DB_ReadOnly) ? 0 : 1;
    }

    DatabasePool::DatabasePool(C4Database* main) : DatabasePool(main->getName(), main->getConfiguration()) {
        _dbTag       = _c4db_getDatabaseTag(main);
        Cache& cache = writeable() ? _readWrite : _readOnly;
        cache.available.emplace_back(main);
        cache.created++;
        logInfo("initial database is %s", nameOf(main).c_str());
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
        unique_lock lock(_mutex);
        if ( _closed ) error::_throw(error::NotOpen, "DatabasePool is closed");
        Assert(newCapacity >= 1 + _readWrite.capacity, "capacity too small");
        _readOnly.capacity = newCapacity - _readWrite.capacity;
        Assert(_readOnly.capacity >= 1);
        // Toss out any excess RO databases:
        int keep = std::max(0, int(_readOnly.capacity) - int(_readOnly.borrowedCount()));
        while ( _readOnly.available.size() > keep ) closeDB(_readOnly.pop());
    }

    bool DatabasePool::sameAs(C4Database* db) const noexcept {
        return db->getName() == _dbName && db->getConfiguration().parentDirectory == _dbConfig.parentDirectory;
    }

    void DatabasePool::onOpen(std::function<void(C4Database*)> init, bool callNow) {
        unique_lock lock(_mutex);
        _initializer = std::move(init);
        if ( callNow && _initializer ) {
            for ( auto& db : _readOnly.available ) _initializer(db);
            for ( auto& db : _readWrite.available ) _initializer(db);
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
        for ( auto& db : cache.available ) {
            closeDB(std::move(db));
            --cache.created;
        }
        cache.available.clear();
    }

    // Allocates and opens a new C4Database instance.
    Retained<C4Database> DatabasePool::newDB(Cache& cache) {
        auto config             = _dbConfig;
        config.flags            = cache.flags;
        Retained<C4Database> db = C4Database::openNamed(_dbName, config);
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

    void DatabasePool::closeDB(Retained<C4Database> db) noexcept {
        logInfo("closing %s", nameOf(db).c_str());
        try {
            db->close();
        }
        catchAndWarn();
    }

    Retained<C4Database> DatabasePool::Cache::pop() {
        Retained<C4Database> db;
        if ( !available.empty() ) {
            db = std::move(available.back());
            available.pop_back();
        }
        return db;
    }

    BorrowedDatabase DatabasePool::borrow(Cache& cache, bool or_wait) {
        unique_lock lock(_mutex);
        while ( true ) {
            if ( _closed ) error::_throw(error::NotOpen, "DatabasePool is closed");
            Retained<C4Database> dbp = cache.pop();
            if ( !dbp ) {
                if (cache.created < cache.capacity ) {
                    dbp = newDB(cache);
                    ++cache.created;
                } else if (cache.capacity == 0) {
                    Assert(&cache == &_readWrite);
                    error::_throw(error::NotWriteable, "Database is read-only");
                }
            }
            if ( dbp || !or_wait ) return BorrowedDatabase(std::move(dbp), this);

            // Nothing available, so wait and retry
            auto timeout = std::chrono::system_clock::now() + kTimeout;
            if ( _cond.wait_until(lock, timeout) == std::cv_status::timeout )
                error::_throw(error::Busy, "Timed out waiting on DatabasePool::borrow");
        }
    }

    BorrowedDatabase DatabasePool::borrow() { return borrow(_readOnly, true); }

    BorrowedDatabase DatabasePool::tryBorrow() { return borrow(_readOnly, false); }

    BorrowedDatabase DatabasePool::borrowWriteable() { return borrow(_readWrite, true); }

    BorrowedDatabase DatabasePool::tryBorrowWriteable() { return borrow(_readWrite, false); }

    // Called by BorrowedDatabase's destructor and its reset method.
    void DatabasePool::returnDatabase(fleece::Retained<C4Database> db) {
        Assert(db && !db->isInTransaction());
        unique_lock lock(_mutex);
        Cache&      cache = (db->getConfiguration().flags & kC4DB_ReadOnly) ? _readOnly : _readWrite;
        Assert(cache.available.size() < cache.created);
        if ( cache.created <= cache.capacity && !_closed ) {
            cache.available.emplace_back(std::move(db));
        } else {
            // Toss out a DB if capacity was lowered after it was checked out, or I'm closed:
            closeDB(std::move(db));
            --cache.created;
        }
        _cond.notify_all();  // wake up waiting `borrow` and `close` calls
    }

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
        if ( _db && _pool ) _pool->returnDatabase(std::move(_db));
    }

    BorrowedCollection::BorrowedCollection(BorrowedDatabase&& bdb, C4CollectionSpec const& spec)
        : _bdb(std::move(bdb)), _collection(_bdb ? _bdb->getCollection(spec) : nullptr) {
        if ( _bdb && !_collection ) error::_throw(error::NotFound, "no such collection");
    }


}  // namespace litecore
