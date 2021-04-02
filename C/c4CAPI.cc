//
// c4CAPI.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "c4BlobStore.hh"
#include "c4Certificate.hh"
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "c4Listener.hh"
#include "c4Observer.hh"
#include "c4Query.hh"
#include "c4Replicator.hh"

#include "c4.h"
#include "c4ExceptionUtils.hh"
#include "c4Private.h"
#include "c4QueryImpl.hh"
#include "Delimiter.hh"
#include <sstream>

#include "fleece/Mutable.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


#pragma mark - BLOBS:


bool c4blob_keyFromString(C4Slice str, C4BlobKey* outKey) noexcept {
    if (auto key = C4BlobKey::withDigestString(str); key) {
        *outKey = *key;
        return true;
    } else {
        return false;
    }
}


C4SliceResult c4blob_keyToString(C4BlobKey key) noexcept {
    try {
        return C4SliceResult(alloc_slice(key.digestString()));
    } catchError(nullptr);
    return {};
}



C4BlobStore* c4blob_openStore(C4Slice dirPath,
                              C4DatabaseFlags flags,
                              const C4EncryptionKey *key,
                              C4Error* outError) noexcept
{
    try {
        return new C4BlobStore(dirPath, flags, (key ? *key : C4EncryptionKey{}));
    } catchError(outError)
    return nullptr;
}


C4BlobStore* c4db_getBlobStore(C4Database *db, C4Error* outError) noexcept {
    try {
        return &db->getBlobStore();
    } catchError(outError)
    return nullptr;
}


void c4blob_freeStore(C4BlobStore *store) noexcept {
    delete store;
}


bool c4blob_deleteStore(C4BlobStore* store, C4Error *outError) noexcept {
    try {
        store->deleteStore();
        delete store;
        return true;
    } catchError(outError)
    return false;
}


int64_t c4blob_getSize(C4BlobStore* store, C4BlobKey key) noexcept {
    try {
        return store->getSize(key);
    } catchAndWarn()
    return -1;
}


C4SliceResult c4blob_getContents(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        return C4SliceResult(store->getContents(key));
    } catchError(outError)
    return {};
}


C4StringResult c4blob_getFilePath(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        return C4StringResult(store->getFilePath(key));
    } catchError(outError)
    return {};
}


C4BlobKey c4blob_computeKey(C4Slice contents) {
    return C4BlobKey::computeDigestOfContent(contents);
}


bool c4blob_create(C4BlobStore* store,
                   C4Slice contents,
                   const C4BlobKey *expectedKey,
                   C4BlobKey *outKey,
                   C4Error* outError) noexcept
{
    try {
        auto key = store->createBlob(contents, expectedKey);
        if (outKey)
            *outKey = key;
        return true;
    } catchError(outError)
    return false;
}


bool c4blob_delete(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        store->deleteBlob(key);
        return true;
    } catchError(outError)
    return false;
}


#pragma mark  STREAMING READS:


C4ReadStream* c4blob_openReadStream(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        return new C4ReadStream(*store, key);
    } catchError(outError)
    return nullptr;
}


size_t c4stream_read(C4ReadStream* stream, void *buffer, size_t maxBytes, C4Error* outError) noexcept {
    try {
        clearError(outError);
        return stream->read(buffer, maxBytes);
    } catchError(outError)
    return 0;
}


int64_t c4stream_getLength(C4ReadStream* stream, C4Error* outError) noexcept {
    try {
        return stream->getLength();
    } catchError(outError)
    return -1;
}


bool c4stream_seek(C4ReadStream* stream, uint64_t position, C4Error* outError) noexcept {
    try {
        stream->seek(position);
        return true;
    } catchError(outError)
    return false;
}


void c4stream_close(C4ReadStream* stream) noexcept {
    delete stream;
}


#pragma mark  STREAMING WRITES:


C4WriteStream* c4blob_openWriteStream(C4BlobStore* store, C4Error* outError) noexcept {
    return tryCatch<C4WriteStream*>(outError, [&]{ return new C4WriteStream(*store); });
}


bool c4stream_write(C4WriteStream* stream, const void *bytes, size_t length, C4Error* outError) noexcept {
    return tryCatch(outError, [&]{ stream->write(slice(bytes, length)); });
}


uint64_t c4stream_bytesWritten(C4WriteStream* stream) noexcept {
    return stream->bytesWritten();
}


C4BlobKey c4stream_computeBlobKey(C4WriteStream* stream) noexcept {
    return stream->computeBlobKey();
}


bool c4stream_install(C4WriteStream* stream,
                      const C4BlobKey *expectedKey,
                      C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{ stream->install(expectedKey); });
}


void c4stream_closeWriter(C4WriteStream* stream) noexcept {
    delete stream;
}


#pragma mark - COLLECTION:


C4Collection* c4db_getDefaultCollection(C4Database *db) noexcept {
    return db->getDefaultCollection();
}

bool c4db_hasCollection(C4Database *db, C4String name) noexcept {
    return db->hasCollection(name);
}

C4Collection* C4NULLABLE c4db_getCollection(C4Database *db, C4String name) noexcept {
    return tryCatch<C4Collection*>(nullptr, [&]{ return db->getCollection(name).detach(); });
}

C4Collection* c4db_createCollection(C4Database *db, C4String name, C4Error* C4NULLABLE outError) noexcept {
    return tryCatch<C4Collection*>(outError, [&]{ return db->createCollection(name).detach(); });
}

bool c4db_deleteCollection(C4Database *db, C4String name, C4Error* C4NULLABLE outError) noexcept {
    return tryCatch(outError, [&]{ db->deleteCollection(name); });
}

C4StringResult c4db_collectionNames(C4Database *db) noexcept {
    stringstream result;
    delimiter delim(",");
    for (auto &name : db->collectionNames())
        result << delim << name;
    return C4StringResult(alloc_slice(result.str()));
}


C4String c4coll_getName(C4Collection *coll) noexcept {
    return coll->name();
}

C4Database* c4coll_getDatabase(C4Collection *coll) noexcept {
    return coll->database();
}

uint64_t c4coll_getDocumentCount(C4Collection *coll) noexcept {
    return tryCatch<uint64_t>(nullptr, [=]{return coll->getDocumentCount();});
}

C4SequenceNumber c4coll_getLastSequence(C4Collection *coll) noexcept {
    return tryCatch<sequence_t>(nullptr, [=]{return coll->getLastSequence();});
}

C4Document* c4coll_getDoc(C4Collection *coll,
                          C4String docID,
                          bool mustExist,
                          C4DocContentLevel content,
                          C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        Retained<C4Document> doc = coll->getDocument(docID, mustExist, content);
        if (!doc)
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return move(doc).detach();
    });
}

C4Document* c4coll_getDocBySequence(C4Collection *coll,
                                    C4SequenceNumber sequence,
                                    C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        auto doc = coll->getDocumentBySequence(sequence);
        if (!doc)
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return move(doc).detach();
    });
}

C4Document* c4coll_putDoc(C4Collection *coll,
                          const C4DocPutRequest *rq,
                          size_t * C4NULLABLE outCommonAncestorIndex,
                          C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        return coll->putDocument(*rq, outCommonAncestorIndex, outError).detach();
    });
}

C4Document* c4coll_createDoc(C4Collection *coll,
                             C4String docID,
                             C4Slice revBody,
                             C4RevisionFlags revFlags,
                             C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        return coll->createDocument(docID, revBody, revFlags, outError).detach();
    });
}

bool c4coll_purgeDoc(C4Collection *coll,
                     C4String docID,
                     C4Error* C4NULLABLE outError) noexcept
{
    try {
        if (coll->purgeDoc(docID))
            return true;
        else
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
    } catchError(outError)
    return false;
}

bool c4coll_setDocExpiration(C4Collection *coll,
                             C4String docID,
                             C4Timestamp timestamp,
                             C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<bool>(outError, [=]{
        if (coll->setExpiration(docID, timestamp))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return false;
    });
}

C4Timestamp c4coll_getDocExpiration(C4Collection *coll,
                                    C4String docID,
                                    C4Error* C4NULLABLE outError) noexcept
{
    C4Timestamp expiration = -1;
    tryCatch(outError, [&]{
        expiration = coll->getExpiration(docID);
    });
    return expiration;
}

C4Timestamp c4coll_nextDocExpiration(C4Collection *coll) noexcept {
    return tryCatch<uint64_t>(nullptr, [=]{
        return coll->nextDocExpiration();
    });
}

int64_t c4coll_purgeExpiredDocs(C4Collection *coll, C4Error * C4NULLABLE outError) noexcept {
    return tryCatch<int64_t>(outError, [=]{
        return coll->purgeExpiredDocs();
    });
}

bool c4coll_createIndex(C4Collection *coll,
                        C4String name,
                        C4String indexSpecJSON,
                        C4IndexType indexType,
                        const C4IndexOptions* C4NULLABLE indexOptions,
                        C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch(outError, [&]{
        coll->createIndex(name, indexSpecJSON, indexType, indexOptions);
    });
}

bool c4coll_deleteIndex(C4Collection *coll, C4String name, C4Error* C4NULLABLE outError) noexcept {
    return tryCatch(outError, [&]{
        coll->deleteIndex(name);
    });
}

C4SliceResult c4coll_getIndexesInfo(C4Collection* coll, C4Error* C4NULLABLE outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(coll->getIndexesInfo());
    });
}


#pragma mark - DATABASE:


bool c4db_exists(C4String name, C4String inDirectory) noexcept {
    return C4Database::exists(name, inDirectory);
}


bool c4key_setPassword(C4EncryptionKey *outKey, C4String password, C4EncryptionAlgorithm alg) noexcept {
    return tryCatch(nullptr, [=] {
        *outKey = C4EncryptionKeyFromPassword(password, alg);
    });
}


// TODO - Remove deprecated function
C4Database* c4db_open(C4Slice path,
                      const C4DatabaseConfig *configP,
                      C4Error *outError) noexcept
{
    return tryCatch<C4Database*>(outError, [=] {
        return C4Database::openAtPath(path, configP->flags, &configP->encryptionKey).detach();
    });
}


C4Database* c4db_openNamed(C4String name,
                           const C4DatabaseConfig2 *config,
                           C4Error *outError) noexcept
{
    return tryCatch<C4Database*>(outError, [=] {
        return C4Database::openNamed(name, *config).detach();
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
        C4Database::copyFileToPath(sourcePath, destinationPath, *config);
    });
}


bool c4db_copyNamed(C4String sourcePath,
                    C4String destinationName,
                    const C4DatabaseConfig2* config,
                    C4Error* error) noexcept
{
    return tryCatch(error, [=] {
        C4Database::copyNamed(sourcePath, destinationName, *config);
    });
}


bool c4db_close(C4Database* database, C4Error *outError) noexcept {
    if (database == nullptr)
        return true;
    return tryCatch(outError, [=]{return database->close();});
}


bool c4db_delete(C4Database* database, C4Error *outError) noexcept {
    return tryCatch(outError, [=]{return database->closeAndDeleteFile();});
}


// TODO - Remove deprecated function
bool c4db_deleteAtPath(C4Slice dbPath, C4Error *outError) noexcept {
    if (outError)
        *outError = {};     // deleteDatabaseAtPath may return false w/o throwing an exception
    return tryCatch<bool>(outError, [=]{return C4Database::deleteAtPath(dbPath);});
}


bool c4db_deleteNamed(C4String dbName,
                      C4String inDirectory,
                      C4Error *outError) noexcept
{
    if (outError)
        *outError = {};     // deleteNamed may return false w/o throwing an exception
    return tryCatch<bool>(outError, [=]{return C4Database::deleteNamed(dbName, inDirectory);});
}


// TODO - Remove deprecated function
bool c4db_compact(C4Database* database, C4Error *outError) noexcept {
    return c4db_maintenance(database, kC4Compact, outError);
}


bool c4db_maintenance(C4Database* database, C4MaintenanceType type, C4Error *outError) noexcept {
    return tryCatch(outError, [=]{return database->maintenance(type);});
}


// semi-deprecated
C4Timestamp c4db_nextDocExpiration(C4Database *db) noexcept {
    return c4coll_nextDocExpiration(db->getDefaultCollection());
}


// semi-deprecated
int64_t c4db_purgeExpiredDocs(C4Database *db, C4Error *outError) noexcept {
    return c4coll_purgeExpiredDocs(db->getDefaultCollection(), outError);
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) noexcept {
    return tryCatch(outError, [=]{return database->rekey(newKey);});
}


C4String c4db_getName(C4Database *database) noexcept {
    return slice(database->name());
}

C4SliceResult c4db_getPath(C4Database *database) noexcept {
    return C4SliceResult(database->path());
}


const C4DatabaseConfig* c4db_getConfig(C4Database *database) noexcept {
    return &database->configV1();
}


const C4DatabaseConfig2* c4db_getConfig2(C4Database *database) noexcept {
    return &database->configuration();
}


// semi-deprecated
uint64_t c4db_getDocumentCount(C4Database* database) noexcept {
    return c4coll_getDocumentCount(database->getDefaultCollection());
}


// semi-deprecated
C4SequenceNumber c4db_getLastSequence(C4Database* database) noexcept {
    return c4coll_getLastSequence(database->getDefaultCollection());
}


bool c4db_getUUIDs(C4Database* database, C4UUID *publicUUID, C4UUID *privateUUID,
                   C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        if (publicUUID)
            *publicUUID = database->publicUUID();
        if (privateUUID)
            *privateUUID = database->privateUUID();
    });
}


C4StringResult c4db_getPeerID(C4Database* database) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]{
        return C4StringResult(database->getPeerID());
    });
}


C4ExtraInfo c4db_getExtraInfo(C4Database *database) noexcept {
    return database->extraInfo;
}

void c4db_setExtraInfo(C4Database *database, C4ExtraInfo x) noexcept {
    database->extraInfo = x;
}


bool c4db_isInTransaction(C4Database* database) noexcept {
    return database->isInTransaction();
}


bool c4db_beginTransaction(C4Database* database,
                           C4Error *outError) noexcept
{
    return tryCatch(outError, [=]{database->beginTransaction();});
}

bool c4db_endTransaction(C4Database* database,
                         bool commit,
                         C4Error *outError) noexcept
{
    return tryCatch(outError, [=]{database->endTransaction(commit);});
}


// semi-deprecated
bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) noexcept {
    return c4coll_purgeDoc(database->getDefaultCollection(), docID, outError);
}


bool c4_shutdown(C4Error *outError) noexcept {
    return tryCatch(outError, [] {
        C4Database::shutdownLiteCore();
    });
}


C4SliceResult c4db_rawQuery(C4Database *database, C4String query, C4Error *outError) noexcept {
    try {
        return C4SliceResult(database->rawQuery(query));
    } catchError(outError)
    return {};
}
// LCOV_EXCL_STOP


// only used by tests
bool c4db_findDocAncestors(C4Database *database,
                           unsigned numDocs,
                           unsigned maxAncestors,
                           bool requireBodies,
                           C4RemoteID remoteDBID,
                           const C4String docIDs[], const C4String revIDs[],
                           C4StringResult ancestors[],
                           C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        vector<slice> vecDocIDs((const slice*)&docIDs[0], (const slice*)&docIDs[numDocs]);
        vector<slice> vecRevIDs((const slice*)&revIDs[0], (const slice*)&revIDs[numDocs]);
        auto vecAncestors = database->getDefaultCollection()
                                    ->findDocAncestors(vecDocIDs, vecRevIDs,
                                                       maxAncestors, requireBodies,
                                                       remoteDBID);
        for (unsigned i = 0; i < numDocs; ++i)
        ancestors[i] = C4SliceResult(vecAncestors[i]);
    });
}



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
        C4RawDocument *rawDoc = nullptr;
        database->getRawDocument(storeName, key, [&rawDoc](C4RawDocument *r) {
            if (r) {
                rawDoc = new C4RawDocument{slice(r->key).copy(),
                                           slice(r->meta).copy(),
                                           slice(r->body).copy()};
            }
        });
        if (!rawDoc)
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
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
    return tryCatch(outError, [=]{
        database->putRawDocument(storeName, {key, meta, body});
    });
}


// semi-deprecated
bool c4db_createIndex(C4Database *database,
                      C4Slice name,
                      C4Slice indexSpecJSON,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    return c4coll_createIndex(database->getDefaultCollection(), name, indexSpecJSON,
                              indexType, indexOptions, outError);
}


// semi-deprecated
bool c4db_deleteIndex(C4Database *database,
                      C4Slice name,
                      C4Error *outError) noexcept
{
    return c4coll_deleteIndex(database->getDefaultCollection(), name, outError);
}

C4SliceResult c4db_getIndexes(C4Database* database, C4Error* outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(database->getIndexesInfo(false));
    });
}


// semi-deprecated
C4SliceResult c4db_getIndexesInfo(C4Database* database, C4Error* outError) noexcept {
    return c4coll_getIndexesInfo(database->getDefaultCollection(), outError);
}


C4SliceResult c4db_getIndexRows(C4Database* database, C4String indexName, C4Error* outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(database->getIndexRows(indexName));
    });
}


C4StringResult c4db_getCookies(C4Database *db,
                               C4Address request,
                               C4Error *outError) noexcept
{
    return tryCatch<C4StringResult>(outError, [=]() {
        C4StringResult result(db->getCookies(request));
        if (!result.buf)
            clearError(outError);
        return result;
    });
}


bool c4db_setCookie(C4Database *db,
                    C4String setCookieHeader,
                    C4String fromHost,
                    C4String fromPath,
                    C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [=]() {
        if (db->setCookie(setCookieHeader, fromHost, fromPath))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("Invalid cookie"), outError);
        return false;
    });
}


void c4db_clearCookies(C4Database *db) noexcept {
    tryCatch(nullptr, [db]() {
        db->clearCookies();
    });
}


#pragma mark - DOCUMENT:


C4Document* c4doc_retain(C4Document *doc) noexcept {
    return retain(doc);
}


void c4doc_release(C4Document *doc) noexcept {
   release(doc);
}


// semi-deprecated
C4Document* c4db_getDoc(C4Database *database,
                       C4Slice docID,
                       bool mustExist,
                       C4DocContentLevel content,
                       C4Error *outError) noexcept
{
    return c4coll_getDoc(database->getDefaultCollection(), docID, mustExist, content, outError);
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError) noexcept
{
    return c4db_getDoc(database, docID, mustExist, kDocGetCurrentRev, outError);
}


// semi-deprecated
C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError) noexcept
{
    return c4coll_getDocBySequence(database->getDefaultCollection(), sequence, outError);
}


// semi-deprecated
bool c4doc_setExpiration(C4Database *db, C4Slice docId, C4Timestamp timestamp, C4Error *outError) noexcept {
    return c4coll_setDocExpiration(db->getDefaultCollection(), docId, timestamp, outError);
}


// semi-deprecated
C4Timestamp c4doc_getExpiration(C4Database *db, C4Slice docID, C4Error *outError) noexcept {
    return c4coll_getDocExpiration(db->getDefaultCollection(), docID, outError);
}


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (doc->selectRevision(revID, withBody))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return false;
    });
}


bool c4doc_selectCurrentRevision(C4Document* doc) noexcept {
    return doc->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (doc->loadRevisionBody())
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return false;
    });
}


bool c4doc_hasRevisionBody(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, [=]{return doc->hasRevisionBody();});
}


C4Slice c4doc_getRevisionBody(C4Document* doc) noexcept {
    return doc->getRevisionBody();
}


C4SliceResult c4doc_getSelectedRevIDGlobalForm(C4Document* doc) noexcept {
    return C4SliceResult(doc->getSelectedRevIDGlobalForm());
}


C4SliceResult c4doc_getRevisionHistory(C4Document* doc,
                                       unsigned maxRevs,
                                       const C4String backToRevs[],
                                       unsigned backToRevsCount) noexcept {
    return C4SliceResult(doc->getRevisionHistory(maxRevs,
                                                 (const slice*)backToRevs,
                                                 backToRevsCount));
}


bool c4doc_selectParentRevision(C4Document* doc) noexcept {
    return doc->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, [=]{return doc->selectNextRevision();});
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (doc->selectNextLeafRevision(includeDeleted))
            return true;
        clearError(outError); // normal failure
        return false;
    });
}


bool c4doc_selectCommonAncestorRevision(C4Document* doc, C4String rev1, C4String rev2) noexcept {
    return tryCatch<bool>(nullptr, [&]{
        return doc->selectCommonAncestorRevision(rev1, rev2);
    });
}


bool c4doc_removeRevisionBody(C4Document* doc) noexcept {
    return doc->removeRevisionBody();
}


// this function is probably unused; remove it if so
int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError) noexcept
{
    try {
        return doc->purgeRevision(revID);
    } catchError(outError)
    return -1;
}


C4RemoteID c4db_getRemoteDBID(C4Database *db, C4String remoteAddress, bool canCreate,
                              C4Error *outError) noexcept
{
    return tryCatch<C4RemoteID>(outError, [&]{
        return db->getRemoteDBID(remoteAddress, canCreate);
    });
}


C4SliceResult c4db_getRemoteDBAddress(C4Database *db, C4RemoteID remoteID) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]{
        return C4SliceResult(db->getRemoteDBAddress(remoteID));
    });
}


C4SliceResult c4doc_getRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]{
        return C4SliceResult(doc->remoteAncestorRevID(remoteDatabase));
    });
}


bool c4doc_setRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase, C4String revID,
                             C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        doc->setRemoteAncestorRevID(remoteDatabase, revID);
        return true;
    });
}


// this wrapper is only used by tests
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4String revID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        return database->getDefaultCollection()->markDocumentSynced(docID, revID, sequence, remoteID);
    });
}


char* c4doc_generateID(char *docID, size_t bufferSize) noexcept {
    return C4Document::generateID(docID, bufferSize);
}


// semi-deprecated
C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError) noexcept
{
    return c4coll_putDoc(database->getDefaultCollection(), rq, outCommonAncestorIndex, outError);
}


// semi-deprecated
C4Document* c4doc_create(C4Database *database,
                         C4String docID,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
    return c4coll_createDoc(database->getDefaultCollection(), docID, revBody, revFlags, outError);
}


C4Document* c4doc_update(C4Document *doc,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        Retained<C4Document> updated = doc->update(revBody, revFlags);
        if (!updated)
            c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
        return move(updated).detach();
    });
}


bool c4doc_resolveConflict2(C4Document *doc,
                            C4String winningRevID,
                            C4String losingRevID,
                            FLDict mergedProperties,
                            C4RevisionFlags mergedFlags,
                            C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        doc->resolveConflict(winningRevID, losingRevID, mergedProperties, mergedFlags);
    });
}


bool c4doc_resolveConflict(C4Document *doc,
                           C4String winningRevID,
                           C4String losingRevID,
                           C4Slice mergedBody,
                           C4RevisionFlags mergedFlags,
                           C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        doc->resolveConflict(winningRevID, losingRevID, mergedBody, mergedFlags);
    });
}


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError) noexcept
{
    try {
        if (doc->save(maxRevTreeDepth))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
        return false;
    } catchError(outError)
    return false;
}


/// Returns true if the two ASCII revIDs are equal (though they may not be byte-for-byte equal.)
bool c4rev_equal(C4Slice rev1, C4Slice rev2) noexcept {
    return C4Document::equalRevIDs(rev1, rev2);
}


unsigned c4rev_getGeneration(C4Slice revID) noexcept {
    return C4Document::getRevIDGeneration(revID);
}


C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags) noexcept {
    return C4Document::revisionFlagsFromDocFlags(docFlags);
}


FLDict c4doc_getProperties(C4Document* doc) noexcept {
    return doc->getProperties();
}


C4Document* c4doc_containingValue(FLValue value) {
    return C4Document::containingValue(value);
}


FLEncoder c4db_createFleeceEncoder(C4Database* db) noexcept {
    return db->createFleeceEncoder();
}


FLEncoder c4db_getSharedFleeceEncoder(C4Database* db) noexcept {
    return db->sharedFleeceEncoder();
}


C4SliceResult c4db_encodeJSON(C4Database *db, C4Slice jsonData, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(db->encodeJSON(jsonData));
    });
}


C4SliceResult c4doc_bodyAsJSON(C4Document *doc, bool canonical, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(doc->bodyAsJSON(canonical));
    });
}


FLSharedKeys c4db_getFLSharedKeys(C4Database *db) noexcept {
    return db->getFleeceSharedKeys();
}


bool c4doc_isOldMetaProperty(C4String prop) noexcept {
    return C4Document::isOldMetaProperty(prop);
}


bool c4doc_hasOldMetaProperties(FLDict doc) noexcept {
    return C4Document::hasOldMetaProperties(doc);
}


bool c4doc_getDictBlobKey(FLDict dict, C4BlobKey *outKey) {
    if (auto key = C4Blob::keyFromDigestProperty(dict); key) {
        if (outKey)
            *outKey = *key;
        return true;
    } else {
        return false;
    }
}


bool c4doc_dictIsBlob(FLDict dict, C4BlobKey *outKey) noexcept {
    Assert(outKey);
    if (auto key = C4Blob::keyFromDigestProperty(dict); key && C4Blob::isBlob(dict)) {
        *outKey = *key;
        return true;
    } else {
        return false;
    }
}


C4SliceResult c4doc_getBlobData(FLDict flDict, C4BlobStore *blobStore, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(blobStore->getBlobData(flDict));
    });
}


bool c4doc_dictContainsBlobs(FLDict dict) noexcept {
    return C4Blob::dictContainsBlobs(dict);
}


bool c4doc_blobIsCompressible(FLDict blobDict) {
    return C4Blob::isLikelyCompressible(blobDict);
}


C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc, FLSharedKeys sk, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(C4Document::encodeStrippingOldMetaProperties(doc, sk));
    });
}


#pragma mark - DOC ENUMERATOR:


void c4enum_close(C4DocEnumerator *e) noexcept {
    if (e)
        e->close();
}

void c4enum_free(C4DocEnumerator *e) noexcept {
    delete e;
}


C4DocEnumerator* c4coll_enumerateChanges(C4Collection *collection,
                                         C4SequenceNumber since,
                                         const C4EnumeratorOptions* C4NULLABLE c4options,
                                         C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4DocEnumerator*>(outError, [&]{
        return new C4DocEnumerator(collection, since,
                                   c4options ? *c4options : kC4DefaultEnumeratorOptions);
    });

}

// semi-deprecated
C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                       C4SequenceNumber since,
                                       const C4EnumeratorOptions *c4options,
                                       C4Error *outError) noexcept
{
    return c4coll_enumerateChanges(database->getDefaultCollection(), since, c4options, outError);
}


C4DocEnumerator* c4coll_enumerateAllDocs(C4Collection *collection,
                                         const C4EnumeratorOptions* C4NULLABLE c4options,
                                         C4Error* C4NULLABLE outError) noexcept
{
    return tryCatch<C4DocEnumerator*>(outError, [&]{
        return new C4DocEnumerator(collection,
                                   c4options ? *c4options : kC4DefaultEnumeratorOptions);
    });
}


// semi-deprecated
C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                       const C4EnumeratorOptions *c4options,
                                       C4Error *outError) noexcept
{
    return c4coll_enumerateAllDocs(database->getDefaultCollection(), c4options, outError);
}


bool c4enum_next(C4DocEnumerator *e, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (e->next())
            return true;
        clearError(outError);      // end of iteration is not an error
        return false;
    });
}


bool c4enum_getDocumentInfo(C4DocEnumerator *e, C4DocumentInfo *outInfo) noexcept {
    return e->getDocumentInfo(*outInfo);
}


C4Document* c4enum_getDocument(C4DocEnumerator *e, C4Error *outError) noexcept {
    return tryCatch<C4Document*>(outError, [&]{
        Retained<C4Document> doc = e->getDocument();
        if (!doc)
            clearError(outError);      // end of iteration is not an error
        return move(doc).detach();
    });
}


#pragma mark - OBSERVERS:


// semi-deprecated
C4DatabaseObserver* c4dbobs_create(C4Database *db,
                                   C4DatabaseObserverCallback callback,
                                   void *context) noexcept
{
    return c4dbobs_createOnCollection(db->getDefaultCollection(), callback, context);
}


C4DatabaseObserver* c4dbobs_createOnCollection(C4Collection* coll,
                                               C4DatabaseObserverCallback callback,
                                               void* C4NULLABLE context) noexcept
{
    return C4CollectionObserver::create(coll, [=](C4DatabaseObserver *obs) {
        callback(obs, context);
    }).release();
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4DatabaseChange outChanges[],
                            uint32_t maxChanges,
                            bool *outExternal) noexcept
{
    static_assert(sizeof(C4DatabaseChange) == sizeof(C4DatabaseObserver::Change),
                  "C4DatabaseChange doesn't match C4DatabaseObserver::Change");
    return tryCatch<uint32_t>(nullptr, [&]{
        memset(outChanges, 0, maxChanges * sizeof(C4DatabaseChange));
        return obs->getChanges((C4DatabaseObserver::Change*)outChanges,
                               maxChanges, outExternal);
        // This is slightly sketchy because C4DatabaseObserver::Change contains alloc_slices, whereas
        // C4DatabaseChange contains slices. The result is that the docID and revID memory will be
        // temporarily leaked, since the alloc_slice destructors won't be called.
        // For this purpose we have c4dbobs_releaseChanges(), which does the same sleight of hand
        // on the array but explicitly destructs each Change object, ensuring its alloc_slices are
        // destructed and the backing store's ref-count goes back to what it was originally.
    });
}


void c4dbobs_releaseChanges(C4DatabaseChange changes[], uint32_t numChanges) noexcept {
    for (uint32_t i = 0; i < numChanges; ++i) {
        auto &change = (C4DatabaseObserver::Change&)changes[i];
        change.~Change();
    }
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    delete obs;
}


// semi-deprecated
C4DocumentObserver* c4docobs_create(C4Database *db,
                                    C4Slice docID,
                                    C4DocumentObserverCallback callback,
                                    void *context) noexcept
{
    return c4docobs_createWithCollection(db->getDefaultCollection(), docID, callback, context);
}


C4DocumentObserver* c4docobs_createWithCollection(C4Collection *coll,
                                                  C4String docID,
                                                  C4DocumentObserverCallback callback,
                                                  void* C4NULLABLE context) noexcept
{
    return tryCatch<unique_ptr<C4DocumentObserver>>(nullptr, [&]{
        auto fn = [=](C4DocumentObserver *obs, fleece::slice docID, C4SequenceNumber seq) {
            callback(obs, docID, seq, context);
        };
        return C4DocumentObserver::create(coll, docID, fn);
    }).release();
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    delete obs;
}


#pragma mark - QUERY:


C4Query* c4query_new2(C4Database *database,
                      C4QueryLanguage language,
                      C4Slice expression,
                      int *outErrorPos,
                      C4Error *outError) noexcept
{
    if (outErrorPos)
        *outErrorPos = -1;
    return tryCatch<C4Query*>(outError, [&]{
        C4Query *query = database->newQuery(language, expression, outErrorPos).detach();
        if (!query)
            c4error_return(LiteCoreDomain, kC4ErrorInvalidQuery, {}, outError);
        return query;
    });
}


// deprecated
C4Query* c4query_new(C4Database *database, C4String expression, C4Error *error) noexcept {
    return c4query_new2(database, kC4JSONQuery, expression, nullptr, error);
}


unsigned c4query_columnCount(C4Query *query) noexcept {
    return query->columnCount();
}


FLString c4query_columnTitle(C4Query *query, unsigned column) noexcept {
    return query->columnTitle(column);
}


void c4query_setParameters(C4Query *query, C4String encodedParameters) noexcept {
    query->setParameters(encodedParameters);
}


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *c4options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        return query->createEnumerator(c4options, encodedParameters);
    });
}


C4StringResult c4query_explain(C4Query *query) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]{
        return C4StringResult(query->explain());
    });
}


C4SliceResult c4query_fullTextMatched(C4Query *query,
                                      const C4FullTextMatch *term,
                                      C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(query->fullTextMatched(*term));
    });
}


#pragma mark - QUERY ENUMERATOR API:


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (asInternal(e)->next())
            return true;
        clearError(outError);      // end of iteration is not an error
        return false;
    });
}


bool c4queryenum_seek(C4QueryEnumerator *e,
                      int64_t rowIndex,
                      C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        asInternal(e)->seek(rowIndex);
        return true;
    });
}


int64_t c4queryenum_getRowCount(C4QueryEnumerator *e,
                                 C4Error *outError) noexcept
{
    try {
        return asInternal(e)->getRowCount();
    } catchError(outError)
    return -1;
}



C4QueryEnumerator* c4queryenum_refresh(C4QueryEnumerator *e,
                                       C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        clearError(outError);
        return asInternal(e)->refresh();
    });
}


C4QueryEnumerator* c4queryenum_retain(C4QueryEnumerator *e) noexcept {
    return retain(asInternal(e));
}


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        asInternal(e)->close();
    }
}

void c4queryenum_release(C4QueryEnumerator *e) noexcept {
    release(asInternal(e));
}


#pragma mark - QUERY OBSERVER API:


C4QueryObserver* c4queryobs_create(C4Query *query, C4QueryObserverCallback cb, void *ctx) noexcept {
    auto fn = [cb,ctx](C4QueryObserver *obs) {
        cb(obs, obs->query(), ctx);
    };
    return new C4QueryObserverImpl(query, fn);
}

void c4queryobs_setEnabled(C4QueryObserver *obs, bool enabled) noexcept {
    obs->setEnabled(enabled);
}

void c4queryobs_free(C4QueryObserver* obs) noexcept {
    delete obs;
}

C4QueryEnumerator* c4queryobs_getEnumerator(C4QueryObserver *obs,
                                            bool forget,
                                            C4Error *outError) noexcept
{
    return asInternal(obs)->getEnumeratorImpl(forget, outError).detach();
}


#pragma mark - CERTIFICATE API: (EE)


#ifdef COUCHBASE_ENTERPRISE


C4Cert* c4cert_createRequest(const C4CertNameComponent *nameComponents,
                             size_t nameCount,
                             C4CertUsage certUsages,
                             C4KeyPair *subjectKey,
                             C4Error *outError) noexcept
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        vector<C4CertNameComponent> components(&nameComponents[0], &nameComponents[nameCount]);
        return C4Cert::createRequest(components, certUsages, subjectKey).detach();
    });
}


C4Cert* c4cert_fromData(C4Slice certData, C4Error *outError) noexcept {
    return tryCatch<C4Cert*>(outError, [&]() {
        return C4Cert::fromData(certData).detach();
    });
}


C4Cert* c4cert_requestFromData(C4Slice certRequestData, C4Error *outError) noexcept {
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        return C4Cert::requestFromData(certRequestData).detach();
    });
}


C4SliceResult c4cert_copyData(C4Cert* cert, bool pemEncoded) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(cert->data(pemEncoded));
    });
}


C4StringResult c4cert_subjectName(C4Cert* cert) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        return C4StringResult(cert->subjectName());
    });
}


C4StringResult c4cert_subjectNameComponent(C4Cert* cert, C4CertNameAttributeID attrID) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]() {
        return C4StringResult(cert->subjectNameComponent(attrID));
    });
}


bool c4cert_subjectNameAtIndex(C4Cert* cert,
                               unsigned index,
                               C4CertNameInfo *outInfo) noexcept
{
    auto info = cert->subjectNameAtIndex(index);
    if (!info.id)
        return false;
    outInfo->id = FLSliceResult(move(info.id));
    outInfo->value = FLSliceResult(move(info.value));
    return true;
}


C4CertUsage c4cert_usages(C4Cert* cert) noexcept {
    return cert->usages();
}


C4StringResult c4cert_summary(C4Cert* cert) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4StringResult(cert->summary());
    });
}


void c4cert_getValidTimespan(C4Cert* cert,
                             C4Timestamp *outCreated,
                             C4Timestamp *outExpires)
{
    pair<C4Timestamp,C4Timestamp> ts;
    try {
        ts = cert->validTimespan();
    } catch (...) {
        ts.first = ts.second = 0;
    }
    if (outCreated)
        *outCreated = ts.first;
    if (outExpires)
        *outExpires = ts.second;
}


bool c4cert_isSigned(C4Cert* cert) noexcept {
    return cert->isSigned();
}


bool c4cert_isSelfSigned(C4Cert* cert) noexcept {
    return cert->isSelfSigned();
}


C4Cert* c4cert_signRequest(C4Cert *c4Cert,
                           const C4CertIssuerParameters* C4NULLABLE c4Params,
                           C4KeyPair *issuerPrivateKey,
                           C4Cert *issuerC4Cert,
                           C4Error *outError) noexcept
{
    return tryCatch<C4Cert*>(outError, [&]() -> C4Cert* {
        if (!c4Params)
            c4Params = &kDefaultCertIssuerParameters;
        return c4Cert->signRequest(*c4Params, issuerPrivateKey, issuerC4Cert).detach();
    });
}


bool c4cert_sendSigningRequest(C4Cert *c4Cert,
                               C4Address address,
                               C4Slice optionsDictFleece,
                               C4CertSigningCallback callback,
                               void *context,
                               C4Error *outError) noexcept
{
    return tryCatch(outError, [&] {
        c4Cert->sendSigningRequest(address, optionsDictFleece, [=](C4Cert *cert, C4Error error) {
            callback(context, cert, error);
        });
    });
}


C4KeyPair* c4cert_getPublicKey(C4Cert* cert) noexcept {
    return tryCatch<C4KeyPair*>(nullptr, [&]() -> C4KeyPair* {
        return cert->publicKey().detach();
    });
}


C4KeyPair* c4cert_loadPersistentPrivateKey(C4Cert* cert, C4Error *outError) noexcept {
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        return cert->loadPersistentPrivateKey().detach();
    });
}


C4Cert* c4cert_nextInChain(C4Cert* cert) noexcept {
    return tryCatch<C4Cert*>(nullptr, [&]() -> C4Cert* {
        return cert->nextInChain().detach();
    });
}

C4SliceResult c4cert_copyChainData(C4Cert* cert) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(cert->chainData());
    });
}


bool c4cert_save(C4Cert *cert,
                 bool entireChain,
                 C4String name,
                 C4Error *outError)
{
    return tryCatch(outError, [&]() {
        if (cert)
            cert->save(entireChain, name);
        else
            C4Cert::deleteNamed(name);
    });
}


C4Cert* c4cert_load(C4String name,
                    C4Error *outError)
{
    return tryCatch<C4Cert*>(outError, [&]() {
        return C4Cert::load(name).detach();
    });
}


#pragma mark - KEY PAIR API: (EE)


C4KeyPair* c4keypair_generate(C4KeyPairAlgorithm algorithm,
                              unsigned sizeInBits,
                              bool persistent,
                              C4Error *outError) noexcept
{
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        return C4KeyPair::generate(algorithm, sizeInBits, persistent).detach();
    });
}


C4KeyPair* c4keypair_fromPublicKeyData(C4Slice publicKeyData, C4Error *outError) noexcept {
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return C4KeyPair::fromPublicKeyData(publicKeyData).detach();
    });
}


C4KeyPair* c4keypair_fromPrivateKeyData(C4Slice data, C4Slice password, C4Error *outError) noexcept
{
    return tryCatch<C4KeyPair*>(outError, [&]() {
        return C4KeyPair::fromPrivateKeyData(data, password).detach();
    });
}


C4KeyPair* c4keypair_persistentWithPublicKey(C4KeyPair* key, C4Error *outError) noexcept {
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        return C4KeyPair::persistentWithPublicKey(key).detach();
    });
}


bool c4keypair_hasPrivateKey(C4KeyPair* key) noexcept {
    return key->hasPrivateKey();
}


bool c4keypair_isPersistent(C4KeyPair* key) noexcept {
    return key->isPersistent();
}


C4SliceResult c4keypair_publicKeyDigest(C4KeyPair* key) noexcept {
    return C4SliceResult(key->publicKeyDigest());
}


C4SliceResult c4keypair_publicKeyData(C4KeyPair* key) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(key->publicKeyData());
    });
}


C4SliceResult c4keypair_privateKeyData(C4KeyPair* key) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]() {
        return C4SliceResult(key->privateKeyData());
    });
}


bool c4keypair_removePersistent(C4KeyPair* key, C4Error *outError) noexcept {
    return tryCatch(outError, [&]() {
        key->removePersistent();
    });
}


C4KeyPair* c4keypair_fromExternal(C4KeyPairAlgorithm algorithm,
                                  size_t keySizeInBits,
                                  void *externalKey,
                                  C4ExternalKeyCallbacks callbacks,
                                  C4Error *outError)
{
    return tryCatch<C4KeyPair*>(outError, [&]() -> C4KeyPair* {
        return C4KeyPair::fromExternal(algorithm, keySizeInBits, externalKey, callbacks).detach();
    });
}


#endif // COUCHBASE_ENTERPRISE


#pragma mark - LISTENER API: (EE)


C4ListenerAPIs c4listener_availableAPIs(void) noexcept {
    return C4Listener::availableAPIs();
}


C4Listener* c4listener_start(const C4ListenerConfig *config, C4Error *outError) noexcept {
    try {
        return new C4Listener(*config);
    } catchError(outError)
    return nullptr;
}


void c4listener_free(C4Listener *listener) noexcept {
    delete listener;
}


C4StringResult c4db_URINameFromPath(C4String pathSlice) noexcept {
    try {
        if (string name = C4Listener::URLNameFromPath(pathSlice); name.empty())
        return {};
        else
            return FLSliceResult(alloc_slice(name));
    } catchAndWarn()
    return {};
}


bool c4listener_shareDB(C4Listener *listener, C4String name, C4Database *db,
                        C4Error *outError) noexcept
{
    try {
        return listener->shareDB(name, db);
    } catchError(outError);
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4Database *db,
                          C4Error *outError) noexcept
{
    try {
        if (listener->unshareDB(db))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotOpen, "Database not shared"_sl, outError);
    } catchError(outError);
    return false;
}


uint16_t c4listener_getPort(C4Listener *listener) noexcept {
    try {
        return listener->port();
    } catchAndWarn()
    return 0;
}


FLMutableArray c4listener_getURLs(C4Listener *listener, C4Database *db,
                                  C4ListenerAPIs api, C4Error* err) noexcept {
    try {
        auto urls = fleece::MutableArray::newArray();
        for (string &url : listener->URLs(db, api))
            urls.append(url);
        return (FLMutableArray)FLValue_Retain(urls);
    } catchError(err);
    return nullptr;
}


void c4listener_getConnectionStatus(C4Listener *listener,
                                    unsigned *connectionCount,
                                    unsigned *activeConnectionCount) noexcept
{
    auto [conns, active] = listener->connectionStatus();
    if (connectionCount)
        *connectionCount = conns;
    if (activeConnectionCount)
        *activeConnectionCount = active;
}
