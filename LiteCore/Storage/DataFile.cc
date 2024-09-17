//
// DataFile.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//
#include "DataFile.hh"
#include "DataFile+Shared.hh"
#include "Query.hh"
#include "DocumentKeys.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "fleece/RefCounted.hh"
#include "Stopwatch.hh"
#include "Instrumentation.hh"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <exception>
#include <iomanip>
#include <sstream>
#include <thread>

#include "SQLiteDataFile.hh"

using namespace std;

namespace litecore {

    // How long deleteDataFile() should wait for other threads to close their connections
    static const unsigned kOtherDBCloseTimeoutSecs = 6;


    LogDomain DBLog("DB");

    unordered_map<string, DataFile::Shared*> DataFile::Shared::sFileMap;
    mutex                                    DataFile::Shared::sFileMapMutex;


#pragma mark - FACTORY:

    void DataFile::Factory::moveFile(const FilePath& from, const FilePath& to) {
        auto fromBaseLen = from.fileName().size();
        from.forEachMatch([&](const FilePath& f) {
            string toFile = to.fileName() + from.fileName().substr(fromBaseLen);
            f.moveTo(to.dirName() + toFile);
        });
    }

    bool DataFile::Factory::fileExists(const FilePath& path) { return path.exists(); }

    std::vector<DataFile::Factory*> DataFile::factories() { return {&SQLiteDataFile::sqliteFactory()}; }

    DataFile::Factory* DataFile::factoryNamed(const std::string& name) {
        auto facs = factories();
        if ( name.empty() ) return facs[0];
        for ( auto factory : facs )
            if ( name == factory->name() ) return factory;
        return nullptr;
    }

    DataFile::Factory* DataFile::factoryNamed(const char* name) {
        if ( !name ) name = "";
        return factoryNamed(string(name));
    }

    DataFile::Factory* DataFile::factoryForFile(const FilePath& path) {
        auto ext = path.extension();
        for ( auto factory : factories() )
            if ( ext == factory->filenameExtension() ) return factory;
        return nullptr;
    }

    // collection utilities:

    size_t DataFile::findCollectionPathSeparator(const string& collectionPath, size_t pos) {
        for ( auto i = pos; i < collectionPath.length(); ++i ) {
            if ( collectionPath.at(i) == '\\' ) {
                ++i;
            } else if ( collectionPath.at(i) == KeyStore::kScopeCollectionSeparator ) {
                return i;
            }
        }
        return string::npos;
    }

    string DataFile::unescapeCollectionName(const string& unescaped) {
        stringstream ss{string{KeyStore::kScopeCollectionSeparator} + unescaped + KeyStore::kScopeCollectionSeparator};
        string       ret;
        while ( true ) {
            string res;
            ss >> std::quoted(res, KeyStore::kScopeCollectionSeparator, '\\');
            ret += res;
            if ( ss.rdbuf()->in_avail() > 0 ) {
                ret += '.';
                ss.unget();
            } else {
                break;
            }
        }
        return ret;
    }

    std::pair<alloc_slice, alloc_slice> DataFile::splitCollectionPath(const string& collectionPath) {
        auto        dot = DataFile::findCollectionPathSeparator(collectionPath);
        alloc_slice scope;
        alloc_slice collection;
        if ( dot == string::npos ) {
            collection = DataFile::unescapeCollectionName(collectionPath);
        } else {
            scope      = DataFile::unescapeCollectionName(collectionPath.substr(0, dot));
            collection = DataFile::unescapeCollectionName(collectionPath.substr(dot + 1));
        }
        return std::make_pair(scope, collection);
    }

#pragma mark - DATAFILE:


    const DataFile::Options DataFile::Options::defaults = {
            {true},                    // sequences
            true,   true, true, true,  // create, writeable, useDocumentKeys, upgradeable
            false                      // diskSyncFull
    };

    DataFile::DataFile(const FilePath& path, Delegate* delegate, const DataFile::Options* options)
        : Logging(DBLog), _delegate(delegate), _path(path), _options(options ? *options : Options::defaults) {
        // Do this last so I'm fully constructed before other threads can see me (#425)
        _shared = Shared::forPath(path, this);
    }

    DataFile::~DataFile() {
        logDebug("destructing (~DataFile)");
        Assert(!_inTransaction);
        if ( _shared ) _shared->removeDataFile(this);
    }

    string DataFile::loggingIdentifier() const { return _path.path(); }

    uint64_t DataFile::fileSize() { return filePath().dataSize(); }

    void DataFile::close(bool forDelete) {
        // https://github.com/couchbase/couchbase-lite-core/issues/776
        // Need to fulfill two opposing conditions simultaneously
        // 1. The data file must remain in shared until it is fully closed
        //    so that delete operations will not delete it while it is being
        //    closed.
        // 2. The data file must indicate that it is no longer valid so that
        //    other classes with interest in the data file do not continue to
        //    operate on it
        _closeSignaled = true;

        closeAllQueries();

        for ( auto& i : _keyStores ) { i.second->close(); }
        _close(forDelete);
        if ( _shared->removeDataFile(this) ) logInfo("Closing database");
    }

    void DataFile::reopen() {
        logInfo("File=%s Opening database", _shared->loggingName().c_str());
        for ( auto& i : _keyStores ) {
            // CBL-859 If we have rekeyed, then the keystores all have invalid compiled statements
            // inside of them.  Close them all so that those get cleared, and then the next call to
            // getKeyStore will reopen them on demand
            i.second->close();
        }

        _shared->addDataFile(this);
    }

    void DataFile::checkOpen() const {
        if ( !isOpen() ) error::_throw(error::NotOpen);
    }

    DataFile* DataFile::openAnother(Delegate* delegate) { return factory().openFile(_path, delegate, &_options); }

    void DataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
        if ( alg != kNoEncryption ) error::_throw(error::UnsupportedEncryption);
    }

    void DataFile::forOtherDataFiles(function_ref<void(DataFile*)> fn) { _shared->forOpenDataFiles(this, fn); }

    Retained<RefCounted> DataFile::sharedObject(const string& key) { return _shared->sharedObject(key); }

    Retained<RefCounted> DataFile::addSharedObject(const string& key, RefCounted* object) {
        return _shared->addSharedObject(key, object);
    }

#pragma mark - DELETION:

    //#define FAIL_FAST

    void DataFile::deleteDataFile() { deleteDataFile(this, nullptr, _shared, factory()); }

    bool DataFile::Factory::deleteFile(const FilePath& path, const Options* options) {
        Retained<Shared> shared = Shared::forPath(path, nullptr);
        return DataFile::deleteDataFile(nullptr, options, shared, *this);
    }

    bool DataFile::deleteDataFile(DataFile* file, const Options* options, Shared* shared, Factory& factory) {
        shared->condemn(true);
        try {
            // Fail-fast if there is open db connection owned by the external application.
#ifdef FAIL_FAST
            shared->forOpenDataFiles(file, [](DataFile* df) {
                if ( df->databaseTag() == kDatabaseTagAppOpened ) {
                    error::_throw(error::Busy, "Can't delete db file while the caller has open connections");
                }
            });
#endif

            // c.f. C4_ENUM(uint32_t, C4DatabaseTag) in DataFile.hh
            const char* const kDatabaseTags[] = {
                    "appOpened",          "dbAccess",           "c4RemoteReplicator", "c4IncomingReplicator",
                    "c4LocalReplicator1", "c4LocalReplicator2", "backgroundDB",       "RESTListener"};
            static_assert(sizeof(kDatabaseTags) / sizeof(const char*) == kDatabaseTag_RESTListener + 1);

            // Wait for other connections to close -- in multithreaded setups there may be races where
            // another thread takes a bit longer to close its connection.
            int               n = 0;
            fleece::Stopwatch st;
            for ( ;; ) {
                auto otherConnections = (long)shared->openCount();
                if ( file && file->isOpen() ) --otherConnections;
                Assert(otherConnections >= 0);
                if ( otherConnections == 0 ) break;

                if ( n++ == 0 )
                    LogTo(DBLog, "Waiting for %ld other connection(s) to close before deleting %s", otherConnections,
                          shared->path.c_str());
                if ( st.elapsed() > kOtherDBCloseTimeoutSecs ) {
                    ostringstream ss;
                    ss << "Can't delete db file while other connections are open. The open connections are tagged ";
                    bool first = true;
                    shared->forOpenDataFiles(nullptr, [&](DataFile* df) {
                        if ( !first ) {
                            ss << ", ";
                        } else {
                            first = false;
                        }
                        ss << kDatabaseTags[df->databaseTag()];
                    });
                    error::_throw(error::Busy, "%s.", ss.str().c_str());
                } else {
                    std::this_thread::sleep_for(100ms);
                }
            }

            if ( file ) file->close(true);
            bool result = factory._deleteFile(FilePath(shared->path), options);
            shared->condemn(false);
            return result;
        } catch ( ... ) {
            shared->condemn(false);
            throw;
        }
    }

#pragma mark - KEY-STORES:


    const string DataFile::kDefaultKeyStoreName{"default"};
    const string DataFile::kInfoKeyStoreName{"info"};

    KeyStore& DataFile::getKeyStore(slice name) const { return getKeyStore(name, _options.keyStores); }

    KeyStore& DataFile::getKeyStore(slice name, KeyStore::Capabilities options) const {
        checkOpen();
        string nameStr(name);
        auto   i = _keyStores.find(nameStr);
        if ( i != _keyStores.end() ) {
            KeyStore& store = *i->second;
            store.reopen();
            return store;
        } else {
            return const_cast<DataFile*>(this)->addKeyStore(nameStr, options);
        }
    }

    KeyStore& DataFile::addKeyStore(const string& name, KeyStore::Capabilities options) {
        logDebug("open KVS '%s'", name.c_str());
        checkOpen();
        Assert(!(options.sequences && !_options.keyStores.sequences),
               "KeyStore can't have sequences if Database doesn't");
        KeyStore* store  = newKeyStore(name, options);
        _keyStores[name] = unique_ptr<KeyStore>(store);
        return *store;
    }

    void DataFile::closeKeyStore(const string& name) {
        logDebug("close KVS '%s'", name.c_str());
        auto i = _keyStores.find(name);
        if ( i != _keyStores.end() ) {
            // Never remove a KeyStore from _keyStores: there may be objects pointing to it
            i->second->close();
        }
    }

    KeyStore& DataFile::defaultKeyStore(KeyStore::Capabilities options) const {
        checkOpen();
        if ( !_defaultKeyStore ) _defaultKeyStore = &getKeyStore(kDefaultKeyStoreName, options);
        return *_defaultKeyStore;
    }

    void DataFile::forOpenKeyStores(function_ref<void(KeyStore&)> fn) {
        for ( auto& ks : _keyStores ) fn(*ks.second);
    }

    fleece::impl::SharedKeys* DataFile::documentKeys() const {
        std::call_once(_documentKeysOnce, [this] {
            auto keys = _documentKeys.get();
            if ( !keys && _options.useDocumentKeys ) {
                auto mutableThis = const_cast<DataFile*>(this);
                keys             = new DocumentKeys(*mutableThis);
                _documentKeys    = keys;
            }
        });
        return _documentKeys.get();
    }

#pragma mark - QUERIES:

    void DataFile::registerQuery(Query* query) {
        unique_lock<mutex> lock(_queriesMutex);
        _queries.insert(query);
    }

    void DataFile::unregisterQuery(Query* query) {
        unique_lock<mutex> lock(_queriesMutex);
        _queries.erase(query);
    }

    void DataFile::closeAllQueries() {
        unique_lock<mutex> lock(_queriesMutex);
        for ( auto& query : _queries ) query->close();
        _queries.clear();
    }

#pragma mark - TRANSACTION:

    void DataFile::beginTransactionScope(ExclusiveTransaction* t) {
        Assert(!_inTransaction);
        checkOpen();
        _shared->setTransaction(t);
        _inTransaction = true;
    }

    void DataFile::transactionBegan(ExclusiveTransaction*) {
        if ( documentKeys() ) _documentKeys->transactionBegan();
    }

    void DataFile::transactionEnding(ExclusiveTransaction*, bool committing) {
        // Save changes to shared keys:
        if ( _documentKeys ) {
            if ( committing ) _documentKeys->save();
            else
                _documentKeys->revert();
        }

        // Notify key-stores so they can save state:
        forOpenKeyStores([committing](KeyStore& ks) { ks.transactionWillEnd(committing); });
    }

    void DataFile::endTransactionScope(ExclusiveTransaction* t) {
        _shared->unsetTransaction(t);
        _inTransaction = false;
        if ( _documentKeys ) _documentKeys->transactionEnded();
    }

    ExclusiveTransaction& DataFile::transaction() {
        Assert(_inTransaction);
        return *_shared->transaction();
    }

    void DataFile::withFileLock(function_ref<void(void)> fn) {
        if ( _inTransaction ) {
            fn();
        } else {
            ExclusiveTransaction t(this, false);
            fn();
        }
    }

    ExclusiveTransaction::ExclusiveTransaction(DataFile* db) : ExclusiveTransaction(db, true) {}

    ExclusiveTransaction::ExclusiveTransaction(DataFile* db, bool active) : _db(*db), _active(false) {
        _db.beginTransactionScope(this);
        if ( active ) {
            _db._logVerbose("begin transaction");
            Signpost::begin(Signpost::transaction, uintptr_t(this));
            _db._beginTransaction(this);
            _active = true;
            _db.transactionBegan(this);
        }
    }

    void ExclusiveTransaction::commit() {
        Assert(_active, "Transaction is not active");
        _db.transactionEnding(this, true);
        _active = false;
        _db._logVerbose("commit transaction");
        Stopwatch st;
        _db._endTransaction(this, true);
        auto elapsed = st.elapsed();
        Signpost::end(Signpost::transaction, uintptr_t(this));
        if ( elapsed >= 0.1 ) _db._logInfo("Committing transaction took %.3f sec", elapsed);
    }

    void ExclusiveTransaction::abort() {
        Assert(_active, "Transaction is not active");
        _db.transactionEnding(this, false);
        _active = false;
        _db._logVerbose("abort transaction");
        _db._endTransaction(this, false);
        Signpost::end(Signpost::transaction, uintptr_t(this));
    }

    void ExclusiveTransaction::notifyCommitted(SequenceTracker& sequenceTracker) {
        _db.forOtherDataFiles([&](DataFile* other) {
            if ( other->delegate() ) other->delegate()->externalTransactionCommitted(sequenceTracker);
        });
    }

    ExclusiveTransaction::~ExclusiveTransaction() {
        if ( _active ) {
            if ( !std::uncaught_exceptions() )
                _db._logInfo("Transaction exiting scope without explicit commit; aborting");
            abort();
        }
        _db.endTransactionScope(this);
    }

    ReadOnlyTransaction::ReadOnlyTransaction(DataFile* db) {
        db->beginReadOnlyTransaction();
        _db = db;
    }

    ReadOnlyTransaction::~ReadOnlyTransaction() {
        if ( _db ) {
            try {
                _db->endReadOnlyTransaction();
            } catch ( ... ) { _db->warn("~ReadOnlyTransaction caught C++ exception in endReadOnlyTransaction"); }
        }
    }

}  // namespace litecore
