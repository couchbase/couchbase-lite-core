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
#include "Error.hh"
#include "FilePath.hh"

namespace litecore {
    using namespace std;
    using namespace fleece;

    DatabasePool::DatabasePool(slice name, C4DatabaseConfig2 const& config)
        : _dbName(name), _dbConfig(config), _dbDir(_dbConfig.parentDirectory)
    {
        _dbConfig.parentDirectory = _dbDir;
        _dbConfig.flags &= ~kC4DB_Create;
    }


    DatabasePool::DatabasePool(C4Database* main)
        : DatabasePool(main->getName(), main->getConfiguration()) {
        if ( _dbConfig.flags & kC4DB_ReadOnly ) {
            _readonly.push_back(main);
            _roTotal = 1;
        } else {
            _readwrite = main;
            _rwTotal   = 1;
            _dbConfig.flags |= kC4DB_ReadOnly;
        }
    }

    DatabasePool::~DatabasePool() {
        closeAll();
        if ( _readwrite ) _readwrite->close();
    }

    FilePath DatabasePool::databasePath() const { return FilePath{_dbDir, _dbName + kC4DatabaseFilenameExtension}; }

    unsigned DatabasePool::capacity() const noexcept {
        unique_lock lock(_mutex);
        return _roCapacity + _rwCapacity;
    }

    void DatabasePool::setCapacity(unsigned newCapacity) {
        if ( newCapacity < 2 ) throw invalid_argument("capacity must be at least 2");
        unique_lock lock(_mutex);
        // internally I don't include writeable db in capacity:
        _roCapacity = newCapacity - 1;
        // Toss out any excess RO databases:
        int keep = std::max(0, int(_roCapacity) - int(_roTotal - _readonly.size()));
        while ( _readonly.size() > keep ) _readonly.pop_back();
    }

    bool DatabasePool::sameAs(C4Database* db) const noexcept {
        return db->getName() == _dbName && db->getConfiguration().parentDirectory == _dbConfig.parentDirectory;
    }

    void DatabasePool::onOpen(std::function<void(C4Database*)> init, bool callNow) noexcept {
        unique_lock lock(_mutex);
        _initializer = std::move(init);
        if ( callNow && _initializer ) {
            if ( _readwrite ) _initializer(_readwrite);
            for ( auto& db : _readonly ) _initializer(db);
        }
    }

    unsigned DatabasePool::openCount() const noexcept {
        unique_lock lock(_mutex);
        return _roTotal + _rwTotal;
    }

    unsigned DatabasePool::borrowedCount() const noexcept {
        unique_lock lock(_mutex);
        return _borrowed_count();
    }

    unsigned DatabasePool::_borrowed_count() const {
        return unsigned(_roTotal - _readonly.size()) + (_rwTotal - !!_readwrite);
    }

    void DatabasePool::closeAll() {
        unique_lock lock(_mutex);
        _closeUnused();
        _cond.wait(lock, [&] { return _borrowed_count() == 0; });
        _closeUnused();
        Assert(_roTotal == 0);
    }

    void DatabasePool::closeUnused() {
        unique_lock lock(_mutex);
        _closeUnused();
    }

    void DatabasePool::_closeUnused() {
        for ( auto i = _readonly.begin(); i != _readonly.end(); ) {
            (*i)->close();
            i = _readonly.erase(i);
            --_roTotal;
        }
    }

    // Allocates a new C4Database.
    Retained<C4Database> DatabasePool::newDB() {
        Retained<C4Database> db = C4Database::openNamed(_dbName, _dbConfig);
        if ( _initializer ) _initializer(db);
        return db;
    }

    BorrowedDatabase DatabasePool::borrow(bool or_wait) {
        unique_lock lock(_mutex);
        while ( true ) {
            Retained<C4Database> dbp;
            if ( !_readonly.empty() ) {
                dbp = std::move(_readonly.back());
                _readonly.pop_back();
            } else if ( _roTotal < _roCapacity ) {
                dbp = newDB();
                ++_roTotal;
            }
            if ( dbp ) {
                return BorrowedDatabase(dbp, *this);
            } else if ( !or_wait ) {
                return {nullptr, *this};
            }
            // Nothing available, so wait
            _cond.wait(lock);
        }
    }

    BorrowedDatabase DatabasePool::borrowWriteable(bool or_wait) {
        unique_lock          lock(_mutex);
        Retained<C4Database> dbp;
        if ( _rwTotal == 0 ) {
            error::_throw(error::NotWriteable);
        } else if ( _readwrite || or_wait ) {
            // Get the db, waiting if necessary:
            auto timeout = std::chrono::system_clock::now() + 10s;
            bool ok = _cond.wait_until(lock, timeout, [&] { return _readwrite != nullptr; });
            if (!ok)
                error::_throw(error::Busy);
            dbp = std::move(_readwrite);
        }
        return BorrowedDatabase(dbp, *this);
    }

    BorrowedDatabase DatabasePool::borrow() { return borrow(true); }

    BorrowedDatabase DatabasePool::tryBorrow() { return borrow(false); }

    BorrowedDatabase DatabasePool::borrowWriteable() { return borrowWriteable(true); }

    BorrowedDatabase DatabasePool::tryBorrowWriteable() { return borrowWriteable(false); }

    void DatabasePool::returnDatabase(C4Database* dbp) {
        if ( dbp ) {
            Assert(!dbp->isInTransaction());
            unique_lock lock(_mutex);
            if ( dbp->getConfiguration().flags & kC4DB_ReadOnly ) {
                Assert(_readonly.size() < _roTotal);
                if ( _roTotal <= _roCapacity ) {
                    _readonly.emplace_back(dbp);
                    _cond.notify_all();
                } else {
                    // Toss out a DB if capacity was lowered after it was checked out:
                    delete dbp;
                    --_roTotal;
                }
            } else {
                Assert(_rwTotal == 1);
                Assert(!_readwrite);
                _readwrite = dbp;
                _cond.notify_all();
            }
        }
    }

    BorrowedDatabase& BorrowedDatabase::operator=(BorrowedDatabase&& b) noexcept {
        std::swap(_pool, b._pool);
        std::swap(_db, b._db);
        return *this;
    }

    void BorrowedDatabase::reset() {
        if ( _pool ) {
            _pool->returnDatabase(_db);
            _pool = nullptr;
            _db   = nullptr;
        }
    }


}  // namespace litecore
