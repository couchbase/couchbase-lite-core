//
// c4Database.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Database.hh"
#include "c4BlobStore.hh"
#include "c4Internal.hh"
#include "c4Observer.hh"
#include "c4Private.h"

#include "Database.hh"
#include "Document.hh"
#include "KeyStore.hh"
#include "PrebuiltCopier.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "RevTree.hh"               // just for kDefaultRemoteID
#include "SQLiteDataFile.hh"

#include "SecureRandomize.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "SecureSymmetricCrypto.hh"
#include "StringUtil.hh"

#include "Dict.hh"
#include "Encoder.hh"
#include "JSONConverter.hh"
#include <inttypes.h>

using namespace fleece;
using namespace std;


CBL_CORE_API const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API C4StorageEngine const kC4SQLiteStorageEngine   = "SQLite";


namespace c4Internal {
    Database* asInternal(C4Database *db) {
        return db->_db;
    }
}


static FilePath dbPath(slice name, slice parentDir) {
    Assert(name && parentDir);
    return FilePath(string(parentDir), string(name)).addingExtension(kC4DatabaseFilenameExtension);
}


static void ensureConfigDirExists(const C4DatabaseConfig2 &config) {
    if (!(config.flags & kC4DB_ReadOnly))
        (void)FilePath(string(slice(config.parentDirectory)), "").mkdir();
}


static C4DatabaseConfig newToOldConfig(const C4DatabaseConfig2 &config2) {
    return C4DatabaseConfig {
        config2.flags | kC4DB_AutoCompact,
        NULL,
        (config2.flags & kC4DB_VersionVectors) ? kC4VectorVersioning : kC4TreeVersioning,
        config2.encryptionKey
    };
}


optional<C4EncryptionKey> C4EncryptionKeyFromPassword(slice password, C4EncryptionAlgorithm alg) {
    C4EncryptionKey key;
    if (password && alg != kC4EncryptionNone
                 && litecore::DeriveKeyFromPassword(password, key.bytes, kEncryptionKeySize[alg])) {
        key.algorithm = alg;
        return key;
    } else {
        return nullopt;
    }
}


#pragma mark - C4DATABASE METHODS:


C4Database::C4Database(slice path, C4DatabaseConfig config)
:_db(new Database(FilePath(path), config))
{ }


C4Database::~C4Database() {
    destructExtraInfo(extraInfo);
}


bool C4Database::fileExists(slice name, slice inDirectory) {
    return dbPath(name, inDirectory).exists();
}


Retained<C4Database> C4Database::open(slice name, const C4DatabaseConfig2 &config) {
    ensureConfigDirExists(config);
    FilePath path = dbPath(name, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    return new C4Database(alloc_slice(path), oldConfig);
}


Retained<C4Database> C4Database::open(slice path,
                                      C4DatabaseFlags flags,
                                      const C4EncryptionKey *key)
{
    C4DatabaseConfig config = {flags};
    if (key)
        config.encryptionKey = *key;
    return new C4Database(path, config);
}


Retained<C4Database> C4Database::openAgain() {
    return open(name(), config());
}



void C4Database::copyFile(slice sourcePath, slice destinationName, const C4DatabaseConfig2 &config) {
    ensureConfigDirExists(config);
    FilePath from(sourcePath);
    FilePath to = dbPath(destinationName, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    CopyPrebuiltDB(from, to, &oldConfig);
}


void C4Database::copyFileToPath(slice sourcePath, slice destinationPath,
                                const C4DatabaseConfig &config)
{
    return CopyPrebuiltDB(FilePath(sourcePath), FilePath(destinationPath), &config);
}


bool C4Database::deleteFile(slice path) {
    return Database::deleteDatabaseAtPath(FilePath(path));
}


bool C4Database::deleteFile(slice name, slice inDirectory) {
    return Database::deleteDatabaseAtPath(dbPath(name, inDirectory));
}


void C4Database::shutdownLiteCore() {
    SQLiteDataFile::shutdown();
}


void C4Database::close()                            {_db->close();}
void C4Database::closeAndDeleteFile()               {_db->deleteDatabase();}
void C4Database::rekey(const C4EncryptionKey *key)  {_db->rekey(key);}
void C4Database::maintenance(C4MaintenanceType t)   {_db->maintenance(DataFile::MaintenanceType(t));}

static_assert(int(kC4Compact) == int(DataFile::kCompact));
static_assert(int(kC4FullOptimize) == int(DataFile::kFullOptimize));

void C4Database::lockClientMutex() noexcept         {_db->lockClientMutex();}
void C4Database::unlockClientMutex() noexcept       {_db->unlockClientMutex();}


#pragma mark - ACCESSORS:


uint64_t C4Database::getDocumentCount() const                {return _db->countDocuments();}
C4SequenceNumber C4Database::getLastSequence() const         {return _db->lastSequence();}

slice C4Database::name() const noexcept                      {return _db->name();}
alloc_slice C4Database::path() const                         {return alloc_slice(_db->path());}
const C4DatabaseConfig2& C4Database::config() const noexcept {return *_db->config();}
const C4DatabaseConfig& C4Database::configV1() const noexcept{return *_db->configV1();}


alloc_slice C4Database::peerIDString() const {
    char buf[32];
    sprintf(buf, "%" PRIx64, _db->myPeerID());
    return alloc_slice(buf);
}


C4UUID C4Database::publicUUID() const {
    Database::UUID uuid = _db->getUUID(Database::kPublicUUIDKey);
    return (C4UUID&)uuid;
}


C4UUID C4Database::privateUUID() const {
    Database::UUID uuid = _db->getUUID(Database::kPrivateUUIDKey);
    return (C4UUID&)uuid;
}


#pragma mark - TRANSACTIONS:


bool C4Database::isInTransaction() const noexcept   {return _db->inTransaction();}
void C4Database::beginTransaction()                 {_db->beginTransaction();}
void C4Database::endTransaction(bool commit)        {_db->endTransaction(commit);}


#pragma mark - DOCUMENTS:


Retained<C4Document> C4Database::getDocument(slice docID,
                                             bool mustExist,
                                             C4DocContentLevel content) const
{
    auto doc = _db->documentFactory().newDocumentInstance(docID, ContentOption(content));
    if (mustExist && doc && !doc->exists())
        doc = nullptr;
    return doc;
}


Retained<C4Document> C4Database::getDocumentBySequence(C4SequenceNumber sequence) const {
    if (Record rec = _db->defaultKeyStore().get(sequence, kEntireBody); rec.exists())
        return _db->documentFactory().newDocumentInstance(move(rec));
    else
        return nullptr;
}


Retained<C4Document> C4Database::putDocument(const C4DocPutRequest &rq,
                                           size_t *outCommonAncestorIndex,
                                           C4Error *outError)
{
    return _db->putDocument(rq, outCommonAncestorIndex, outError);
}


std::vector<alloc_slice> C4Database::findAncestors(const std::vector<slice> &docIDs,
                                                   const std::vector<slice> &revIDs,
                                                   unsigned maxAncestors,
                                                   bool mustHaveBodies,
                                                   C4RemoteID remoteDBID) const
{
    return _db->documentFactory().findAncestors(docIDs, revIDs, maxAncestors,
                                                mustHaveBodies, remoteDBID);
}


bool C4Database::purgeDocument(slice docID) {
    return _db->purgeDocument(docID);
}


bool C4Database::getRawDocument(slice storeName, slice key, function_ref<void(C4RawDocument*)> cb) {
    Record r = _db->getRawRecord(toString(storeName), key);
    if (r.exists()) {
        C4RawDocument rawDoc = {r.key(), r.version(), r.body()};
        cb(&rawDoc);
        return true;
    } else {
        cb(nullptr);
        return false;
    }
}


void C4Database::putRawDocument(slice storeName, const C4RawDocument &doc) {
    Transaction t(this);
    _db->putRawRecord(toString(storeName), doc.key, doc.meta, doc.body);
    t.commit();
}


static const char * kRemoteDBURLsDoc = "remotes";


C4RemoteID C4Database::getRemoteDBID(slice remoteAddress, bool canCreate) {
    using namespace fleece::impl;
    bool inTransaction = false;
    C4RemoteID remoteID = 0;

    // Make two passes: In the first, just look up the "remotes" doc and look for an ID.
    // If the ID isn't found, then do a second pass where we either add the remote URL
    // or create the doc from scratch, in a transaction.
    for (int creating = 0; creating <= 1; ++creating) {
        if (creating) {     // 2nd pass takes place in a transaction
            _db->beginTransaction();
            inTransaction = true;
        }

        // Look up the doc in the db, and the remote URL in the doc:
        Record doc = _db->getRawRecord(toString(kC4InfoStore), slice(kRemoteDBURLsDoc));
        const Dict *remotes = nullptr;
        remoteID = 0;
        if (doc.exists()) {
            auto body = Value::fromData(doc.body());
            if (body)
                remotes = body->asDict();
            if (remotes) {
                auto idObj = remotes->get(remoteAddress);
                if (idObj)
                    remoteID = C4RemoteID(idObj->asUnsigned());
            }
        }

        if (remoteID > 0) {
            // Found the remote ID!
            return remoteID;
        } else if (!canCreate) {
            break;
        } else if (creating) {
            // Update or create the document, adding the identifier:
            remoteID = 1;
            Encoder enc;
            enc.beginDictionary();
            for (Dict::iterator i(remotes); i; ++i) {
                auto existingID = i.value()->asUnsigned();
                if (existingID) {
                    enc.writeKey(i.keyString());            // Copy existing entry
                    enc.writeUInt(existingID);
                    remoteID = max(remoteID, 1 + C4RemoteID(existingID));   // make sure new ID is unique
                }
            }
            enc.writeKey(remoteAddress);                       // Add new entry
            enc.writeUInt(remoteID);
            enc.endDictionary();
            alloc_slice body = enc.finish();

            // Save the doc:
            _db->putRawRecord(toString(kC4InfoStore), slice(kRemoteDBURLsDoc), nullslice, body);
            _db->endTransaction(true);
            inTransaction = false;
            break;
        }
    }
    if (inTransaction)
        _db->endTransaction(false);
    return remoteID;
}


alloc_slice C4Database::getRemoteDBAddress(C4RemoteID remoteID) {
    using namespace fleece::impl;
    Record doc = _db->getRawRecord(toString(kC4InfoStore), slice(kRemoteDBURLsDoc));
    if (doc.exists()) {
        auto body = Value::fromData(doc.body());
        if (body) {
            for (Dict::iterator i(body->asDict()); i; ++i) {
                if (i.value()->asInt() == remoteID)
                    return alloc_slice(i.keyString());
            }
        }
    }
    return nullslice;
}


bool C4Database::markDocumentSynced(slice docID,
                                    slice revID,
                                    C4SequenceNumber sequence,
                                    C4RemoteID remoteID)
{
    if (remoteID == RevTree::kDefaultRemoteID) {
        // Shortcut: can set kSynced flag on the record to mark that the current revision is
        // synced to remote #1. But the call will return false if the sequence no longer
        // matches, i.e this revision is no longer current. Then have to take the slow approach.
        if (_db->defaultKeyStore().setDocumentFlag(docID, sequence,
                                                   DocumentFlags::kSynced,
                                                   _db->transaction())) {
            return true;
        }
    }

    // Slow path: Load the doc and update the remote-ancestor info in the rev tree:
    Retained<C4Document> doc = getDocument(docID, true, kDocGetAll);
    if (!doc)
        return false;
    if (!revID) {
        // Look up revID by sequence, if it wasn't given:
        Assert(sequence != 0);
        do {
            if (doc->selectedRev.sequence == sequence) {
                revID = slice(doc->selectedRev.revID);
                break;
            }
        } while (doc->selectNextRevision());
        if (!revID)
            return false;
    }
    doc->setRemoteAncestorRevID(remoteID, revID);
    doc->save();
    return true;
}


alloc_slice C4Database::encodeJSON(slice jsonData) const {
    using namespace fleece::impl;
    Encoder &enc = _db->sharedEncoder();
    JSONConverter jc(enc);
    if (!jc.encodeJSON(jsonData)) {
        enc.reset();
        error(error::Fleece, jc.errorCode(), jc.errorMessage())._throw();
    }
    return enc.finish();
}


FLEncoder C4Database::createFleeceEncoder() const {
    FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
    FLEncoder_SetSharedKeys(enc, (FLSharedKeys)_db->documentKeys());
    return enc;
}


FLEncoder C4Database::sharedFleeceEncoder() const   {return _db->sharedFLEncoder();}
FLSharedKeys C4Database::sharedFleeceKeys() const   {return (FLSharedKeys)_db->documentKeys();}


#pragma mark - OBSERVERS:


std::unique_ptr<C4DatabaseObserver> C4Database::observe(DatabaseObserverCallback callback) {
    return C4DatabaseObserver::create(this, callback);
}

std::unique_ptr<C4DocumentObserver> C4Database::observeDocument(slice docID,
                                                                DocumentObserverCallback callback)
{
    return C4DocumentObserver::create(this, docID, callback);
}


#pragma mark - EXPIRATION:


bool C4Database::mayHaveExpiration() const      {return _db->dataFile()->defaultKeyStore().mayHaveExpiration();}
bool C4Database::startHousekeeping()            {return _db->startHousekeeping();}
int64_t C4Database::purgeExpiredDocs()          {return _db->purgeExpiredDocs();}

bool C4Database::setExpiration(slice docID, C4Timestamp timestamp) {return _db->setExpiration(docID, timestamp);}
C4Timestamp C4Database::getExpiration(slice docID) const {return _db->defaultKeyStore().getExpiration(docID);}
C4Timestamp C4Database::nextDocExpiration() const   {return _db->defaultKeyStore().nextExpiration();}


#pragma mark - QUERIES & INDEXES:


alloc_slice C4Database::rawQuery(slice query) {
    return _db->dataFile()->rawQuery(query.asString());
}


void C4Database::createIndex(slice indexName,
                             slice indexSpecJSON,
                             C4IndexType indexType,
                             const C4IndexOptions *indexOptions)
{
    static_assert(sizeof(C4IndexOptions) == sizeof(IndexSpec::Options));

    _db->defaultKeyStore().createIndex(indexName,
                                       indexSpecJSON,
                                       (IndexSpec::Type)indexType,
                                       (const IndexSpec::Options*)indexOptions);
}


void C4Database::deleteIndex(slice indexName) {
    _db->defaultKeyStore().deleteIndex(indexName);
}


alloc_slice C4Database::getIndexesInfo(bool fullInfo) const {
    using namespace fleece::impl;

    Encoder enc;
    enc.beginArray();
    for (const auto &spec : _db->defaultKeyStore().getIndexes()) {
        if (fullInfo) {
            enc.beginDictionary();
            enc.writeKey("name"); enc.writeString(spec.name);
            enc.writeKey("type"); enc.writeInt(spec.type);
            enc.writeKey("expr"); enc.writeString(spec.expressionJSON);
            enc.endDictionary();
        } else {
            enc.writeString(spec.name);
        }
    }
    enc.endArray();
    return enc.finish();
}


alloc_slice C4Database::getIndexRows(slice indexName) const {
    int64_t rowCount;
    alloc_slice rows;
    ((SQLiteDataFile*)_db->dataFile())->inspectIndex(indexName, rowCount, &rows);
    return rows;
}
