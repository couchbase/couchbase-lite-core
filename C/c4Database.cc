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
#include "c4Collection.hh"
#include "c4Query.hh"
#include "c4Private.h"
#include "c4ExceptionUtils.hh"

#include "DatabaseImpl.hh"
#include "DatabaseCookies.hh"
#include "PrebuiltCopier.hh"
#include "SQLiteDataFile.hh"

#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "SecureRandomize.hh"
#include "SecureSymmetricCrypto.hh"
#include "StringUtil.hh"
#include <inttypes.h>
#include <optional>


// NOTE: Most of C4Database is implemented in its concrete subclass DatabaseImpl.


using namespace std;
using namespace fleece;
using namespace litecore;


CBL_CORE_API const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API C4StorageEngine const kC4SQLiteStorageEngine   = "SQLite";


C4EncryptionKey C4EncryptionKeyFromPassword(slice password, C4EncryptionAlgorithm alg) {
    C4EncryptionKey key;
    AssertParam(password.size > 0, "Password is empty");
    AssertParam(alg == kC4EncryptionAES256, "Invalid encryption algorithm");
    if (!litecore::DeriveKeyFromPassword(password, key.bytes, kEncryptionKeySize[alg]))
        C4Error::raise(LiteCoreDomain, kC4ErrorCrypto, "Key derivation failed");
    key.algorithm = alg;
    return key;
}


#pragma mark - STATIC LIFECYCLE METHODS:


static FilePath dbPath(slice name, slice parentDir) {
    if (name.size == 0 || parentDir.size == 0)
        C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter);
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


/*static*/ bool C4Database::deleteDatabaseFileAtPath(const string &dbPath,
                                                     C4StorageEngine storageEngine)
{
    FilePath path(dbPath);
    DataFile::Factory *factory = nullptr;
    if (storageEngine) {
        factory = DataFile::factoryNamed(storageEngine);
        if (!factory)
            Warn("c4db_deleteNamed: unknown storage engine '%s'", storageEngine);
    } else {
        factory = DataFile::factoryForFile(path);
    }
    if (!factory)
        error::_throw(error::WrongFormat);
    return factory->deleteFile(path);
}


/*static*/ bool C4Database::deleteAtPath(slice dbPath) {
    // Find the db file in the bundle:
    FilePath bundle {dbPath, ""};
    if (bundle.exists()) {
        try {
            C4StorageEngine storageEngine = nullptr;
            auto dbFilePath = DatabaseImpl::findOrCreateBundle(bundle.dir(), false, storageEngine);
            // Delete it:
            deleteDatabaseFileAtPath(dbFilePath, storageEngine);
        } catch (const error &x) {
            if (x.code != error::WrongFormat)   // ignore exception if db file isn't found
                throw;
        }
    }
    // Delete the rest of the bundle:
    return bundle.delRecursive();
}


/*static*/ bool C4Database::exists(slice name, slice inDirectory) {
    return dbPath(name, inDirectory).exists();
}


/*static*/ Retained<C4Database> C4Database::openNamed(slice name, const Config &config) {
    ensureConfigDirExists(config);
    FilePath path = dbPath(name, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    return DatabaseImpl::open(path, oldConfig);
}


/*static*/ Retained<C4Database> C4Database::openAtPath(slice path,
                                                       C4DatabaseFlags flags,
                                                       const C4EncryptionKey *key)
{
    C4DatabaseConfig config = {flags};
    if (key)
        config.encryptionKey = *key;
    return DatabaseImpl::open(FilePath(path, ""), config);
}


/*static*/ void C4Database::copyNamed(slice sourcePath, slice destinationName, const Config &config) {
    ensureConfigDirExists(config);
    FilePath from(sourcePath);
    FilePath to = dbPath(destinationName, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    CopyPrebuiltDB(from, to, &oldConfig);
}


/*static*/ void C4Database::copyFileToPath(slice sourcePath, slice destinationPath,
                                           const C4DatabaseConfig &config)
{
    return CopyPrebuiltDB(FilePath(sourcePath), FilePath(destinationPath), &config);
}


/*static*/ bool C4Database::deleteNamed(slice name, slice inDirectory) {
    return deleteAtPath(alloc_slice(dbPath(name, inDirectory)));
}


/*static*/ void C4Database::shutdownLiteCore() {
    SQLiteDataFile::shutdown();
}


C4Database::C4Database(std::string name, std::string dir, const C4DatabaseConfig &inConfig)
:_name(move(name))
,_parentDirectory(move(dir))
,_config{slice(_parentDirectory), inConfig.flags, inConfig.encryptionKey}
,_configV1(inConfig)
{ }


#pragma mark - QUERIES:


Retained<C4Query> C4Database::newQuery(C4QueryLanguage language, slice expr, int *errPos) const {
    return C4Query::newQuery(_defaultCollection, language, expr, errPos);
}


#pragma mark - INDEXES:


void C4Database::createIndex(slice indexName,
                             slice indexSpec,
                             C4QueryLanguage indexSpecLanguage,
                             C4IndexType indexType,
                             const C4IndexOptions *indexOptions)
{
    _defaultCollection->createIndex(indexName, indexSpec, indexSpecLanguage, indexType,
                                    indexOptions);
}


void C4Database::deleteIndex(slice indexName) {
    _defaultCollection->deleteIndex(indexName);
}


alloc_slice C4Database::getIndexesInfo(bool fullInfo) const {
    return _defaultCollection->getIndexesInfo(fullInfo);
}


alloc_slice C4Database::getIndexRows(slice indexName) const {
    return _defaultCollection->getIndexRows(indexName);
}


#pragma mark - COOKIES:


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


#pragma mark - COLLECTIONS:


#ifndef C4_STRICT_COLLECTION_API

#include "c4Document.hh"

// Shims to ease the pain of converting to collections. These delegate to the default collection.

uint64_t C4Database::getDocumentCount() const {
    return getDefaultCollection()->getDocumentCount();
}

C4SequenceNumber C4Database::getLastSequence() const {
    return getDefaultCollection()->getLastSequence();
}

Retained<C4Document> C4Database::getDocument(slice docID,
                                 bool mustExist,
                                 C4DocContentLevel content) const {
    return getDefaultCollection()->getDocument(docID, mustExist, content);
}

Retained<C4Document> C4Database::getDocumentBySequence(C4SequenceNumber sequence) const {
    return getDefaultCollection()->getDocumentBySequence(sequence);
}

Retained<C4Document> C4Database::putDocument(const C4DocPutRequest &rq,
                                 size_t* C4NULLABLE outCommonAncestorIndex,
                                 C4Error *outError) {
    return getDefaultCollection()->putDocument(rq, outCommonAncestorIndex, outError);
}

bool C4Database::purgeDocument(slice docID) {
    return getDefaultCollection()->purgeDocument(docID);
}

C4Timestamp C4Database::getExpiration(slice docID) const {
    return getDefaultCollection()->getExpiration(docID);
}

bool C4Database::setExpiration(slice docID, C4Timestamp timestamp) {
    return getDefaultCollection()->setExpiration(docID, timestamp);
}

#endif // C4_STRICT_COLLECTION_API

