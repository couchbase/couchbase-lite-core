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

#include "c4Internal.hh"
#include "Database.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "SQLiteDataFile.hh"
#include "KeyStore.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Logging.hh"

#include "SecureRandomize.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "PrebuiltCopier.hh"
#include <thread>

using namespace fleece;


CBL_CORE_API const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API C4StorageEngine const kC4SQLiteStorageEngine   = "SQLite";


#pragma mark - C4DATABASE METHODS:


c4Database::~c4Database() {
    FLEncoder_Free(_flEncoder);
}


FLEncoder c4Database::sharedFLEncoder() {
    if (_flEncoder) {
        FLEncoder_Reset(_flEncoder);
    } else {
        _flEncoder = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true, true);
        FLEncoder_SetSharedKeys(_flEncoder, (FLSharedKeys)documentKeys());
    }
    return _flEncoder;
}


bool c4Database::mustUseVersioning(C4DocumentVersioning requiredVersioning,
                                   C4Error *outError) noexcept
{
    if (config.versioning == requiredVersioning)
        return true;
    recordError(LiteCoreDomain, kC4ErrorUnsupported, outError);
    return false;
}


bool c4Database::mustBeInTransaction(C4Error *outError) noexcept {
    if (inTransaction())
        return true;
    recordError(LiteCoreDomain, kC4ErrorNotInTransaction, outError);
    return false;
}

bool c4Database::mustNotBeInTransaction(C4Error *outError) noexcept {
    if (!inTransaction())
        return true;
    recordError(LiteCoreDomain, kC4ErrorTransactionNotClosed, outError);
    return false;
}


#pragma mark - C API:


C4Database* c4db_open(C4Slice path,
                      const C4DatabaseConfig *configP,
                      C4Error *outError) noexcept
{
    return tryCatch<C4Database*>(outError, [=] {
        return retain(new C4Database((string)path, *configP));
    });
}


C4Database* c4db_retain(C4Database* db) {
    return retain(db);
}


C4Database* c4db_openAgain(C4Database* db,
                           C4Error *outError) noexcept
{
    string path = db->path();
    return c4db_open({path.data(), path.size()}, c4db_getConfig(db), outError);
}

bool c4db_copy(C4String sourcePath, C4String destinationPath, const C4DatabaseConfig* config,
               C4Error *error) noexcept {
    return tryCatch(error, [=] {
        FilePath from(slice(sourcePath).asString());
        FilePath to(slice(destinationPath).asString());
        return CopyPrebuiltDB(from, to, config);
    });
}


bool c4db_close(C4Database* database, C4Error *outError) noexcept {
    if (database == nullptr)
        return true;
    return tryCatch(outError, bind(&Database::close, database));
}


bool c4db_free(C4Database* database) noexcept {
    if (database && !database->mustNotBeInTransaction(nullptr))
        return false;
    release(database);
    return true;
}


bool c4db_delete(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::deleteDatabase, database));
}


bool c4db_deleteAtPath(C4Slice dbPath, C4Error *outError) noexcept {
    if (outError)
        *outError = {};     // deleteDatabaseAtPath may return false w/o throwing an exception
    return tryCatch<bool>(outError, bind(&Database::deleteDatabaseAtPath, (string)dbPath));
}


bool c4db_compact(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::compact, database));
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::rekey, database, newKey));
}


C4SliceResult c4db_getPath(C4Database *database) noexcept {
    return sliceResult(database->path().path());
}


const C4DatabaseConfig* c4db_getConfig(C4Database *database) noexcept {
    return &database->config;
}


uint64_t c4db_getDocumentCount(C4Database* database) noexcept {
    return tryCatch<uint64_t>(nullptr, bind(&Database::countDocuments, database));
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) noexcept {
    return tryCatch<sequence_t>(nullptr, bind(&Database::lastSequence, database));
}


uint32_t c4db_getMaxRevTreeDepth(C4Database *database) noexcept {
    return tryCatch<uint32_t>(nullptr, bind(&Database::maxRevTreeDepth, database));
}


void c4db_setMaxRevTreeDepth(C4Database *database, uint32_t depth) noexcept {
    tryCatch(nullptr, bind(&Database::setMaxRevTreeDepth, database, depth));
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


bool c4db_isInTransaction(C4Database* database) noexcept {
    return database->inTransaction();
}


bool c4db_beginTransaction(C4Database* database,
                           C4Error *outError) noexcept
{
    return tryCatch(outError, bind(&Database::beginTransaction, database));
}

bool c4db_endTransaction(C4Database* database,
                         bool commit,
                         C4Error *outError) noexcept
{
    return tryCatch(outError, bind(&Database::endTransaction, database, commit));
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
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
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
        return sliceResult(database->dataFile()->rawQuery(slice(query).asString()));
    } catchError(outError)
    return {};
}
// LCOV_EXCL_STOP

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
        Record r = database->getRawDocument((string)storeName, key);
        if (!r.exists()) {
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
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
                                 bind(&Database::putRawDocument, database, (string)storeName,
                                      key, meta, body));
    c4db_endTransaction(database, commit, outError);
    return commit;
}
