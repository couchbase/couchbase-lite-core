//
//  c4Database.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Internal.hh"
#include "Database.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "ForestDataFile.hh"
#include "SQLiteDataFile.hh"
#include "KeyStore.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Logging.hh"

#include "Collatable.hh"
#include "FilePath.hh"


const char* const kC4ForestDBStorageEngine = "ForestDB";
const char* const kC4SQLiteStorageEngine   = "SQLite";


#pragma mark - C4DATABASE METHODS:


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

/*static*/ bool c4Database::rekeyDataFile(DataFile* database,
                                          const C4EncryptionKey *newKey,
                                          C4Error *outError) noexcept
{
    try {
        Database::rekeyDataFile(database, newKey);
        return true;
    } catchError(outError);
    return false;
}


#pragma mark - C API:


C4Database* c4db_open(C4Slice path,
                      const C4DatabaseConfig *configP,
                      C4Error *outError) noexcept
{
    if (!checkParam(configP != nullptr, outError))
        return nullptr;
    return external( tryCatch<Database*>(outError,
                                            bind(&Database::newDatabase, (string)path, *configP)) );
}


bool c4db_close(C4Database* database, C4Error *outError) noexcept {
    if (database == nullptr)
        return true;
    return tryCatch(outError, bind(&Database::close, database));
}


bool c4db_free(C4Database* database) noexcept {
    if (database == nullptr)
        return true;
    if (!database->mustNotBeInTransaction(nullptr))
        return false;
    database->release();
    return true;
}


bool c4db_delete(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::deleteDatabase, database));
}


bool c4db_deleteAtPath(C4Slice dbPath, const C4DatabaseConfig *config, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::deleteDatabaseAtPath, (string)dbPath, config));
}


bool c4db_compact(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::compact, database));
}


bool c4db_isCompacting(C4Database *database) noexcept {
    return database ? database->dataFile()->isCompacting() : DataFile::isAnyCompacting();
}


void c4db_setOnCompactCallback(C4Database *database, C4OnCompactCallback cb, void *context) noexcept {
    database->setOnCompact([cb,context](bool compacting) {
        cb(context, compacting);
    });
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) noexcept {
    return tryCatch(outError, bind(&Database::rekey, database, newKey));
}


C4SliceResult c4db_getPath(C4Database *database) noexcept {
    FilePath path = database->path();
    slice s = slice(path.path()).copy();  // C4SliceResult must be malloced & adopted by caller
    return {s.buf, s.size};
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


bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) noexcept {
    try {
        if (database->purgeDocument(docID))
            return true;
        else
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
    } catchError(outError)
    return false;
}

uint64_t c4db_nextDocExpiration(C4Database *database) noexcept
{
    return tryCatch<uint64_t>(nullptr, bind(&Database::nextDocumentExpirationTime, database));
}

bool c4_shutdown(C4Error *outError) noexcept {
    return tryCatch(outError, []{
        ForestDataFile::shutdown();
        SQLiteDataFile::shutdown();
    });
}

#pragma mark - RAW DOCUMENTS:


void c4raw_free(C4RawDocument* rawDoc) noexcept {
    if (rawDoc) {
        rawDoc->key.free();
        rawDoc->meta.free();
        rawDoc->body.free();
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
        rawDoc->meta = r.meta().copy();
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
