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
#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "Document.hh"
#include "SQLiteDataFile.hh"
#include "KeyStore.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Logging.hh"

#include "SecureRandomize.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "SecureSymmetricCrypto.hh"
#include "StringUtil.hh"
#include "PrebuiltCopier.hh"
#include <inttypes.h>
#include <thread>

using namespace fleece;
using namespace std;


CBL_CORE_API const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API C4StorageEngine const kC4SQLiteStorageEngine   = "SQLite";


#pragma mark - C4DATABASE METHODS:


C4Database::~C4Database() {
    destructExtraInfo(extraInfo);
}


#pragma mark - C API:


static FilePath dbPath(C4String name, C4String parentDir) {
    Assert(name.buf != nullptr && parentDir.buf != nullptr);
    return FilePath(string(slice(parentDir)), string(slice(name)))
                .addingExtension(kC4DatabaseFilenameExtension);
}


static bool ensureConfigDirExists(const C4DatabaseConfig2 *config, C4Error *outError) {
    if (config->flags & kC4DB_ReadOnly)
        return true;
    try {
        (void)FilePath(string(slice(config->parentDirectory)), "").mkdir();
        return true;
    } catch (const exception &x) {
        recordException(x, outError);
        return false;
    }
}


static C4DatabaseConfig newToOldConfig(const C4DatabaseConfig2 *config2) {
    return C4DatabaseConfig {
        config2->flags | kC4DB_AutoCompact,
        NULL,
        (config2->flags & kC4DB_VersionVectors) ? kC4VectorVersioning : kC4TreeVersioning,
        config2->encryptionKey
    };
}


bool c4db_exists(C4String name, C4String inDirectory) C4API {
    return dbPath(name, inDirectory).exists();
}


bool c4key_setPassword(C4EncryptionKey *key, C4String password, C4EncryptionAlgorithm alg) C4API {
    bool ok = (password.buf && alg != kC4EncryptionNone
                && litecore::DeriveKeyFromPassword(password, key->bytes, kEncryptionKeySize[alg]));
    key->algorithm = ok ? alg : kC4EncryptionNone;
    return ok;
}


// TODO - Remove deprecated function
C4Database* c4db_open(C4Slice path,
                      const C4DatabaseConfig *configP,
                      C4Error *outError) noexcept
{
    return tryCatch<C4Database*>(outError, [=] {
        return retain(new C4Database(toString(path), *configP));
    });
}


C4Database* c4db_openNamed(C4String name,
                           const C4DatabaseConfig2 *config,
                           C4Error *outError) C4API
{
    if (!ensureConfigDirExists(config, outError))
        return nullptr;
    FilePath path = dbPath(name, config->parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    return tryCatch<C4Database*>(outError, [=] {
        return retain(new C4Database(path, oldConfig));
    });
}


C4Database* c4db_openAgain(C4Database* db,
                           C4Error *outError) noexcept
{
    return c4db_openNamed(c4db_getName(db), c4db_getConfig2(db), outError);
}


// TODO - Remove deprecated function
bool c4db_copy(C4String sourcePath, C4String destinationPath, const C4DatabaseConfig* config,
               C4Error *error) noexcept {
    return tryCatch(error, [=] {
        FilePath from(slice(sourcePath).asString());
        FilePath to(slice(destinationPath).asString());
        return CopyPrebuiltDB(from, to, config);
    });
}


bool c4db_copyNamed(C4String sourcePath,
                    C4String destinationName,
                    const C4DatabaseConfig2* config,
                    C4Error* error) C4API
{
    if (!ensureConfigDirExists(config, error))
        return false;
    return tryCatch(error, [=] {
        FilePath from(slice(sourcePath).asString());
        FilePath to = dbPath(destinationName, config->parentDirectory);
        C4DatabaseConfig oldConfig = newToOldConfig(config);
        CopyPrebuiltDB(from, to, &oldConfig);
    });
}


bool c4db_close(C4Database* database, C4Error *outError) noexcept {
    if (database == nullptr)
        return true;
    return tryCatch(outError, [=]{return database->close();});
}


bool c4db_delete(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, [=]{return database->deleteDatabase();});
}


// TODO - Remove deprecated function
bool c4db_deleteAtPath(C4Slice dbPath, C4Error *outError) noexcept {
    if (outError)
        *outError = {};     // deleteDatabaseAtPath may return false w/o throwing an exception
    return tryCatch<bool>(outError, [=]{return Database::deleteDatabaseAtPath(toString(dbPath));});
}


bool c4db_deleteNamed(C4String dbName,
                      C4String inDirectory,
                      C4Error *outError) C4API
{
    if (outError)
        *outError = {};     // deleteDatabaseAtPath may return false w/o throwing an exception
    FilePath path = dbPath(dbName, inDirectory);
    return tryCatch<bool>(outError, bind(&Database::deleteDatabaseAtPath, path));
}


bool c4db_compact(C4Database* database, C4Error *outError) noexcept {
    return c4db_maintenance(database, kC4Compact, outError);
}


bool c4db_maintenance(C4Database* database, C4MaintenanceType type, C4Error *outError) C4API {
    static_assert(int(kC4Compact) == int(DataFile::kCompact));
    static_assert(int(kC4FullOptimize) == int(DataFile::kFullOptimize));
    return tryCatch(outError, [=]{return database->maintenance(DataFile::MaintenanceType(type));});
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) noexcept {
    return tryCatch(outError, [=]{return database->rekey(newKey);});
}


C4String c4db_getName(C4Database *database) C4API {
    return slice(database->name());
}

C4SliceResult c4db_getPath(C4Database *database) noexcept {
    return sliceResult(database->path().path());
}


const C4DatabaseConfig* c4db_getConfig(C4Database *database) noexcept {
    return database->configV1();
}


const C4DatabaseConfig2* c4db_getConfig2(C4Database *database) noexcept {
    return database->config();
}


uint64_t c4db_getDocumentCount(C4Database* database) noexcept {
    return tryCatch<uint64_t>(nullptr, [=]{return database->countDocuments();});
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) noexcept {
    return tryCatch<sequence_t>(nullptr, [=]{return database->lastSequence();});
}


uint32_t c4db_getMaxRevTreeDepth(C4Database *database) noexcept {
    return tryCatch<uint32_t>(nullptr, [=]{return database->maxRevTreeDepth();});
}


void c4db_setMaxRevTreeDepth(C4Database *database, uint32_t depth) noexcept {
    tryCatch(nullptr, [=]{return database->setMaxRevTreeDepth(depth);});
}


bool c4db_getUUIDs(C4Database* database, C4UUID *publicUUID, C4UUID *privateUUID,
                   C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        if (publicUUID) {
            auto uuid = (Database::UUID*)publicUUID;
            *uuid = database->getUUID(Database::kPublicUUIDKey);
        }
        if (privateUUID) {
            auto uuid = (Database::UUID*)privateUUID;
            *uuid = database->getUUID(Database::kPrivateUUIDKey);
        }
    });
}


C4StringResult c4db_getPeerID(C4Database* database) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]{
        char buf[32];
        sprintf(buf, "%" PRIx64, database->myPeerID());
        return C4StringResult( alloc_slice(buf) );
    });
}


C4ExtraInfo c4db_getExtraInfo(C4Database *database) C4API {
    return database->extraInfo;
}

void c4db_setExtraInfo(C4Database *database, C4ExtraInfo x) C4API {
    database->extraInfo = x;
}


bool c4db_isInTransaction(C4Database* database) noexcept {
    return database->inTransaction();
}


bool c4db_beginTransaction(C4Database* database,
                           C4Error *outError) noexcept
{
    return tryCatch(outError, [=]{return database->beginTransaction();});
}

bool c4db_endTransaction(C4Database* database,
                         bool commit,
                         C4Error *outError) noexcept
{
    return tryCatch(outError, [=]{return database->endTransaction(commit);});
}


void c4db_lock(C4Database *db) C4API {
    db->lockClientMutex();
}


void c4db_unlock(C4Database *db) C4API {
    db->unlockClientMutex();
}


bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) noexcept {
    try {
        if (database->purgeDocument(docID))
            return true;
        else
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
    } catchError(outError)
    return false;
}


bool c4_shutdown(C4Error *outError) noexcept {
    return tryCatch(outError, [] {
        SQLiteDataFile::shutdown();
    });
}


C4SliceResult c4db_rawQuery(C4Database *database, C4String query, C4Error *outError) noexcept {
    try {
        return C4SliceResult(database->dataFile()->rawQuery(slice(query).asString()));
    } catchError(outError)
    return {};
}
// LCOV_EXCL_STOP


bool c4db_findDocAncestors(C4Database *database,
                           unsigned numDocs,
                           unsigned maxAncestors,
                           bool requireBodies,
                           C4RemoteID remoteDBID,
                           const C4String docIDs[], const C4String revIDs[],
                           C4StringResult ancestors[],
                           C4Error *outError) C4API
{
    return tryCatch(outError, [&]{
        vector<slice> vecDocIDs((const slice*)&docIDs[0], (const slice*)&docIDs[numDocs]);
        vector<slice> vecRevIDs((const slice*)&revIDs[0], (const slice*)&revIDs[numDocs]);
        auto vecAncestors = database->documentFactory().findAncestors(vecDocIDs, vecRevIDs,
                                                                      maxAncestors, requireBodies,
                                                                      remoteDBID);
        for (unsigned i = 0; i < numDocs; ++i)
            ancestors[i] = C4SliceResult(vecAncestors[i]);
    });
}


#pragma mark - RAW DOCUMENTS:


void c4raw_free(C4RawDocument* rawDoc) noexcept {
    if (rawDoc) {
        ((slice)rawDoc->key).free();
        ((slice)rawDoc->meta).free();
        ((slice)rawDoc->body).free();
        delete rawDoc;
    }
}


C4RawDocument* c4raw_get(C4Database* database,
                         C4Slice storeName,
                         C4Slice key,
                         C4Error *outError) noexcept
{
    return tryCatch<C4RawDocument*>(outError, [&]{
        Record r = database->getRawDocument(toString(storeName), key);
        if (!r.exists()) {
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
            return (C4RawDocument*)nullptr;
        }
        auto rawDoc = new C4RawDocument;
        rawDoc->key = r.key().copy();
        rawDoc->meta = r.version().copy();
        rawDoc->body = r.body().copy();
        return rawDoc;
    });
}


bool c4raw_put(C4Database* database,
               C4Slice storeName,
               C4Slice key,
               C4Slice meta,
               C4Slice body,
               C4Error *outError) noexcept
{
    if (!c4db_beginTransaction(database, outError))
        return false;
    bool commit = tryCatch(outError,
                                 [=]{return database->putRawDocument(toString(storeName),
                                                                     key, meta, body);});
    c4db_endTransaction(database, commit, outError);
    return commit;
}
