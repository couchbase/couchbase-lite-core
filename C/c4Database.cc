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
#include "c4Document.hh"
#include "c4Observer.hh"
#include "c4Private.h"
#include "c4Internal.hh"
#include "c4ExceptionUtils.hh"

#include "DatabaseImpl.hh"
#include "DatabaseCookies.hh"
#include "DocumentFactory.hh"
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

using namespace std;
using namespace fleece;
using namespace litecore;


CBL_CORE_API const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API C4StorageEngine const kC4SQLiteStorageEngine   = "SQLite";


namespace litecore {
    DatabaseImpl* asInternal(const C4Database *db) {
        // Yeah, casting away constness, I know...
        return (DatabaseImpl*)db;
    }
}

#define IMPL asInternal(this)


static FilePath dbPath(slice name, slice parentDir) {
    Assert(name && parentDir);
    return FilePath(string(parentDir), string(name)).addingExtension(kC4DatabaseFilenameExtension);
}


static void ensureConfigDirExists(const C4DatabaseConfig2 &config) {
    if (!(config.flags & kC4DB_ReadOnly))
        (void)FilePath(slice(config.parentDirectory), "").mkdir();
}


static C4DatabaseConfig newToOldConfig(const C4DatabaseConfig2 &config2) {
    return C4DatabaseConfig {
        config2.flags | kC4DB_AutoCompact,
        NULL,
        (config2.flags & kC4DB_VersionVectors) ? kC4VectorVersioning : kC4TreeVersioning,
        config2.encryptionKey
    };
}


C4EncryptionKey C4EncryptionKeyFromPassword(slice password, C4EncryptionAlgorithm alg) {
    C4EncryptionKey key;
    AssertParam(password.size > 0, "Password is empty");
    AssertParam(alg == kC4EncryptionAES256, "Invalid encryption algorithm");
    if (!litecore::DeriveKeyFromPassword(password, key.bytes, kEncryptionKeySize[alg]))
        C4Error::raise(LiteCoreDomain, kC4ErrorCrypto, "Key derivation failed");
    key.algorithm = alg;
    return key;
}


#pragma mark - C4DATABASE METHODS:


C4Database::~C4Database() {
    destructExtraInfo(extraInfo);
}


bool C4Database::exists(slice name, slice inDirectory) {
    return dbPath(name, inDirectory).exists();
}


Retained<C4Database> C4Database::openNamed(slice name, const Config &config) {
    ensureConfigDirExists(config);
    FilePath path = dbPath(name, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    return DatabaseImpl::open(path, oldConfig);
}


Retained<C4Database> C4Database::openAtPath(slice path,
                                            C4DatabaseFlags flags,
                                            const C4EncryptionKey *key)
{
    C4DatabaseConfig config = {flags};
    if (key)
        config.encryptionKey = *key;
    return DatabaseImpl::open(FilePath(path, ""), config);
}


Retained<C4Database> C4Database::openAgain() {
    return openNamed(getName(), getConfig());
}



void C4Database::copyNamed(slice sourcePath, slice destinationName, const Config &config) {
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


bool C4Database::deleteAtPath(slice path) {
    return DatabaseImpl::deleteDatabaseAtPath(FilePath(path));
}


bool C4Database::deleteNamed(slice name, slice inDirectory) {
    return DatabaseImpl::deleteDatabaseAtPath(dbPath(name, inDirectory));
}


void C4Database::shutdownLiteCore() {
    SQLiteDataFile::shutdown();
}


void C4Database::close()                            {IMPL->close();}
void C4Database::closeAndDeleteFile()               {IMPL->deleteDatabase();}
void C4Database::rekey(const C4EncryptionKey *key)  {IMPL->rekey(key);}
void C4Database::maintenance(C4MaintenanceType t)   {IMPL->maintenance(DataFile::MaintenanceType(t));}

static_assert(int(kC4Compact) == int(DataFile::kCompact));
static_assert(int(kC4FullOptimize) == int(DataFile::kFullOptimize));

void C4Database::lockClientMutex() noexcept         {IMPL->lockClientMutex();}
void C4Database::unlockClientMutex() noexcept       {IMPL->unlockClientMutex();}


#pragma mark - ACCESSORS:


C4BlobStore& C4Database::getBlobStore() const                    {return IMPL->getBlobStore();}

slice C4Database::getName() const noexcept                       {return IMPL->name();}
alloc_slice C4Database::path() const                             {return alloc_slice(IMPL->path());}
const C4Database::Config& C4Database::getConfig() const noexcept {return *IMPL->config();}
const C4DatabaseConfig& C4Database::getConfigV1() const noexcept {return *IMPL->configV1();}


alloc_slice C4Database::getPeerID() const {
    char buf[32];
    sprintf(buf, "%" PRIx64, IMPL->myPeerID());
    return alloc_slice(buf);
}


C4UUID C4Database::publicUUID() const   {return IMPL->getUUID(DatabaseImpl::kPublicUUIDKey);}
C4UUID C4Database::privateUUID() const  {return IMPL->getUUID(DatabaseImpl::kPrivateUUIDKey);}


std::vector<std::string> C4Database::collectionNames() const{return IMPL->collectionNames();}
bool C4Database::hasCollection(slice name) const            {return IMPL->hasCollection(name);}
Retained<C4Collection> C4Database::getCollection(slice name) const   {return IMPL->getCollection(name);}
C4Collection* C4Database::getDefaultCollection() const      {return IMPL->getDefaultCollection();}
Retained<C4Collection> C4Database::createCollection(slice name)      {return IMPL->createCollection(name);}
void C4Database::deleteCollection(slice name)               {return IMPL->deleteCollection(name);}

void C4Database::forEachCollection(const function_ref<void(C4Collection*)> &cb) {
    IMPL->forEachCollection(cb);
}



#pragma mark - TRANSACTIONS:


bool C4Database::isInTransaction() const noexcept   {return IMPL->inTransaction();}
void C4Database::beginTransaction()                 {IMPL->beginTransaction();}
void C4Database::endTransaction(bool commit)        {IMPL->endTransaction(commit);}


#pragma mark - DOCUMENTS:


bool C4Database::getRawDocument(slice storeName, slice key, function_ref<void(C4RawDocument*)> cb) {
    Record r = IMPL->getRawRecord(string(storeName), key);
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
    IMPL->putRawRecord(string(storeName), doc.key, doc.meta, doc.body);
    t.commit();
}


alloc_slice C4Database::encodeJSON(slice jsonData) const {
    impl::Encoder &enc = IMPL->sharedEncoder();
    impl::JSONConverter jc(enc);
    if (!jc.encodeJSON(jsonData)) {
        enc.reset();
        error(error::Fleece, jc.errorCode(), jc.errorMessage())._throw();
    }
    return enc.finish();
}


FLEncoder C4Database::createFleeceEncoder() const {
    FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
    FLEncoder_SetSharedKeys(enc, (FLSharedKeys)IMPL->documentKeys());
    return enc;
}


FLEncoder C4Database::getSharedFleeceEncoder() const   {return IMPL->sharedFLEncoder();}
FLSharedKeys C4Database::getFLSharedKeys() const   {return (FLSharedKeys)IMPL->documentKeys();}


#pragma mark - EXPIRATION:


bool C4Database::mayHaveExpiration() const      {return IMPL->dataFile()->defaultKeyStore().mayHaveExpiration();}
C4Timestamp C4Database::nextDocExpiration() const {return IMPL->nextDocExpiration();}


#pragma mark - QUERIES & INDEXES:


alloc_slice C4Database::rawQuery(slice query) {
    return IMPL->dataFile()->rawQuery(query.asString());
}


void C4Database::createIndex(slice indexName,
                             slice indexSpecJSON,
                             C4IndexType indexType,
                             const C4IndexOptions *indexOptions)
{
    static_assert(sizeof(C4IndexOptions) == sizeof(IndexSpec::Options));

    IMPL->defaultKeyStore().createIndex(indexName,
                                       indexSpecJSON,
                                       (IndexSpec::Type)indexType,
                                       (const IndexSpec::Options*)indexOptions);
}


void C4Database::deleteIndex(slice indexName) {
    IMPL->defaultKeyStore().deleteIndex(indexName);
}


alloc_slice C4Database::getIndexesInfo(bool fullInfo) const {
    impl::Encoder enc;
    enc.beginArray();
    for (const auto &spec : IMPL->defaultKeyStore().getIndexes()) {
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
    ((SQLiteDataFile*)IMPL->dataFile())->inspectIndex(indexName, rowCount, &rows);
    return rows;
}


#pragma mark - REPLICATION:


alloc_slice C4Database::getCookies(const C4Address &request) {
    litecore::repl::DatabaseCookies cookies(this);
    string result = cookies.cookiesForRequest(request);
    if (result.empty())
        return {};
    return alloc_slice(result);
}


bool C4Database::setCookie(slice setCookieHeader,
                           slice fromHost,
                           slice fromPath)
{
    litecore::repl::DatabaseCookies cookies(this);
    bool ok = cookies.setCookie(setCookieHeader.asString(),
                                fromHost.asString(),
                                fromPath.asString());
    if (ok)
        cookies.saveChanges();
    return ok;
}


void C4Database::clearCookies() {
    litecore::repl::DatabaseCookies cookies(this);
    cookies.clearCookies();
    cookies.saveChanges();
}


static const char * kRemoteDBURLsDoc = "remotes";


C4RemoteID C4Database::getRemoteDBID(slice remoteAddress, bool canCreate) {
    bool inTransaction = false;
    C4RemoteID remoteID = 0;

    // Make two passes: In the first, just look up the "remotes" doc and look for an ID.
    // If the ID isn't found, then do a second pass where we either add the remote URL
    // or create the doc from scratch, in a transaction.
    for (int creating = 0; creating <= 1; ++creating) {
        if (creating) {     // 2nd pass takes place in a transaction
            IMPL->beginTransaction();
            inTransaction = true;
        }

        // Look up the doc in the db, and the remote URL in the doc:
        Record doc = IMPL->getRawRecord(string(kInfoStore), kRemoteDBURLsDoc);
        const impl::Dict *remotes = nullptr;
        remoteID = 0;
        if (doc.exists()) {
            auto body = impl::Value::fromData(doc.body());
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
            impl::Encoder enc;
            enc.beginDictionary();
            for (impl::Dict::iterator i(remotes); i; ++i) {
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
            IMPL->putRawRecord(string(kInfoStore), kRemoteDBURLsDoc, nullslice, body);
            IMPL->endTransaction(true);
            inTransaction = false;
            break;
        }
    }
    if (inTransaction)
        IMPL->endTransaction(false);
    return remoteID;
}


alloc_slice C4Database::getRemoteDBAddress(C4RemoteID remoteID) {
    Record doc = IMPL->getRawRecord(string(kInfoStore), kRemoteDBURLsDoc);
    if (doc.exists()) {
        auto body = impl::Value::fromData(doc.body());
        if (body) {
            for (impl::Dict::iterator i(body->asDict()); i; ++i) {
                if (i.value()->asInt() == remoteID)
                    return alloc_slice(i.keyString());
            }
        }
    }
    return nullslice;
}


