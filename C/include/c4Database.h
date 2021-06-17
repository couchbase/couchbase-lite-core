//
// c4Database.h
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

#pragma once
#include "c4DatabaseTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Database Databases
        @{ */


    //////// CONFIGURATION:

    /** \name Configuration
        @{ */


    /** Stores a password into a C4EncryptionKey, by using the key-derivation algorithm PBKDF2
        to securely convert the password into a raw binary key.
        @param encryptionKey  The raw key will be stored here.
        @param password  The password string.
        @param alg  The encryption algorithm to use. Must not be kC4EncryptionNone.
        @return  True on success, false on failure */
    bool c4key_setPassword(C4EncryptionKey *encryptionKey,
                           C4String password,
                           C4EncryptionAlgorithm alg) C4API;


    /** Stores a password into a C4EncryptionKey, by using the key-derivation algorithm PBKDF2
        to securely convert the password into a raw binary key.
        @param encryptionKey  The raw key will be stored here.
        @param password  The password string.
        @param alg  The encryption algorithm to use. Must not be kC4EncryptionNone.
        @return  True on success, false on failure */
    bool c4key_setPassword(C4EncryptionKey *encryptionKey,
                           C4String password,
                           C4EncryptionAlgorithm alg) C4API;


    /** @} */

    //////// DATABASE API:

    
    /** \name Lifecycle
        @{ */

    /** Returns true if a database with the given name exists in the directory. */
    bool c4db_exists(C4String name, C4String inDirectory) C4API;


    /** Opens a database given its name (without the ".cblite2" extension) and directory. */
    C4Database* c4db_openNamed(C4String name,
                               const C4DatabaseConfig2 *config,
                               C4Error* C4NULLABLE outError) C4API;

    /** Opens a new handle to the same database file as `db`.
        The new connection is completely independent and can be used on another thread. */
    C4Database* c4db_openAgain(C4Database* db,
                               C4Error* C4NULLABLE outError) C4API;

    /** Copies a prebuilt database from the given source path and places it in the destination
        directory, with the given name. If a database already exists there, it will be overwritten.
        However if there is a failure, the original database will be restored as if nothing
        happened.
        @param sourcePath  The path to the database to be copied.
        @param destinationName  The name (without filename extension) of the database to create.
        @param config  Database configuration (including destination directory.)
        @param error  On failure, error info will be written here.
        @return  True on success, false on failure. */
    bool c4db_copyNamed(C4String sourcePath,
                        C4String destinationName,
                        const C4DatabaseConfig2* config,
                        C4Error* C4NULLABLE error) C4API;

    /** Closes the database. Does not free the handle, although any operation other than
        c4db_release() will fail with an error. */
    bool c4db_close(C4Database* C4NULLABLE database, C4Error* C4NULLABLE outError) C4API;

    /** Closes the database and deletes the file/bundle. Does not free the handle, although any
        operation other than c4db_release() will fail with an error. */
    bool c4db_delete(C4Database* database, C4Error* C4NULLABLE outError) C4API;

    /** Deletes the file(s) for the database with the given name in the given directory.
        All C4Databases at that path must be closed first or an error will result.
        Returns false, with no error, if the database doesn't exist. */
    bool c4db_deleteNamed(C4String dbName,
                          C4String inDirectory,
                          C4Error* C4NULLABLE outError) C4API;


    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database,
                    const C4EncryptionKey * C4NULLABLE newKey,
                    C4Error* C4NULLABLE outError) C4API;

    /** Closes down the storage engines. Must close all databases first.
        You don't generally need to do this, but it can be useful in tests. */
    bool c4_shutdown(C4Error* C4NULLABLE outError) C4API;


    /** @} */
    /** \name Accessors
        @{ */


    /** Returns the name of the database, as given to `c4db_openNamed`.
        This is the filename _without_ the ".cblite2" extension. */
    C4String c4db_getName(C4Database*) C4API;

    /** Returns the path of the database. */
    C4StringResult c4db_getPath(C4Database*) C4API;

    /** Returns the configuration the database was opened with. */
    const C4DatabaseConfig2* c4db_getConfig2(C4Database *database) C4API C4_RETURNS_NONNULL;

#ifndef C4_STRICT_COLLECTION_API

    /** Returns the number of (undeleted) documents in the database. */
    uint64_t c4db_getDocumentCount(C4Database* database) C4API;

    /** Returns the latest sequence number allocated to a revision. */
    C4SequenceNumber c4db_getLastSequence(C4Database* database) C4API;

    /** Returns the timestamp at which the next document expiration should take place,
        or 0 if there are no documents with expiration times. */
    C4Timestamp c4db_nextDocExpiration(C4Database *database) C4API;

    /** Purges all documents that have expired.
        \warning This is generally unnecessary, since the background housekeeping task will do it.
        You might want to call this if you require the purge to happen synchronously, just before
        copying the database file or something like that.)
        @return  The number of documents purged, or -1 on error. */
    int64_t c4db_purgeExpiredDocs(C4Database *db, C4Error* C4NULLABLE) C4API;

#endif // C4_STRICT_COLLECTION_API

    /** Returns the database's public and/or private UUIDs. (Pass NULL for ones you don't want.) */
    bool c4db_getUUIDs(C4Database* database,
                       C4UUID* C4NULLABLE publicUUID, C4UUID* C4NULLABLE privateUUID,
                       C4Error* C4NULLABLE outError) C4API;

    /** Associates an arbitrary pointer with this database instance, for client use.
        For example, this could be a reference to the higher-level object wrapping the database.

        The `destructor` field of the `C4ExtraInfo` can be used to provide a function that will be
        called when the C4Database is freed, so it can free any resources associated with the pointer. */
    void c4db_setExtraInfo(C4Database *database, C4ExtraInfo) C4API;

    /** Returns the C4ExtraInfo associated with this db reference */
    C4ExtraInfo c4db_getExtraInfo(C4Database *database) C4API;


    /** @} */
    /** \name Database Maintenance
        @{ */


    /** Performs database maintenance.
        For more detail, see the descriptions of the \ref C4MaintenanceType enum constants. */
    bool c4db_maintenance(C4Database* database,
                          C4MaintenanceType type,
                          C4Error* C4NULLABLE outError) C4API;


   /** @} */
    /** \name Transactions
        @{ */


    /** Begins a transaction.
        Transactions can nest; only the first call actually creates a database transaction. */
    bool c4db_beginTransaction(C4Database* database,
                               C4Error* C4NULLABLE outError) C4API;

    /** Commits or aborts a transaction. If there have been multiple calls to beginTransaction, it
        takes the same number of calls to endTransaction to actually end the transaction; only the
        last one commits or aborts the database transaction. */
    bool c4db_endTransaction(C4Database* database,
                             bool commit,
                             C4Error* C4NULLABLE outError) C4API;

    /** Is a transaction active? */
    bool c4db_isInTransaction(C4Database* database) C4API;

    
    /** @} */
    /** @} */


    //////// RAW DOCUMENTS (i.e. info or _local)


    /** \defgroup RawDocs Raw Documents
        @{ */


    /** Frees the storage occupied by a raw document. */
    void c4raw_free(C4RawDocument* C4NULLABLE rawDoc) C4API;

    /** Reads a raw document from the database. In Couchbase Lite the store named "info" is used
        for per-database key/value pairs, and the store "_local" is used for local documents. */
    C4RawDocument* c4raw_get(C4Database* database,
                             C4String storeName,
                             C4String docID,
                             C4Error* C4NULLABLE outError) C4API;

    /** Writes a raw document to the database, or deletes it if both meta and body are NULL. */
    bool c4raw_put(C4Database* database,
                   C4String storeName,
                   C4String key,
                   C4String meta,
                   C4String body,
                   C4Error* C4NULLABLE outError) C4API;

    // Store used for database metadata.
    #define kC4InfoStore C4STR("info")

    // Store used for local (non-replicated) documents.
    #define kC4LocalDocStore C4STR("_local")

    /** @} */


    //-------- DEPRECATED API --------

    C4_DEPRECATED("Use c4db_openNamed")
    C4Database* c4db_open(C4String path,
                          const C4DatabaseConfig *config,
                          C4Error* C4NULLABLE outError) C4API;

    C4_DEPRECATED("Use c4db_copyNamed")
    bool c4db_copy(C4String sourcePath,
                   C4String destinationPath,
                   const C4DatabaseConfig* config,
                   C4Error* C4NULLABLE error) C4API;

    C4_DEPRECATED("Use c4db_deleteNamed")
    bool c4db_deleteAtPath(C4String dbPath, C4Error* C4NULLABLE outError) C4API;

    C4_DEPRECATED("Use c4db_getConfig2")
    const C4DatabaseConfig* c4db_getConfig(C4Database*) C4API;

    C4_DEPRECATED("Use c4db_maintenance")
    bool c4db_compact(C4Database* database, C4Error* C4NULLABLE outError) C4API;

    C4_DEPRECATED("Unnecessary")
    static inline bool c4db_mayHaveExpiration(C4Database *db) C4API {return true;}

    C4_DEPRECATED("Unnecessary")
    static inline bool c4db_startHousekeeping(C4Database *db) C4API {return true;}

C4API_END_DECLS
C4_ASSUME_NONNULL_END
