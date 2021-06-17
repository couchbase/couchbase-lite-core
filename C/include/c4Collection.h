//
//  c4Collection.h
//
// Copyright (c) 2021 Couchbase, Inc All rights reserved.
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
#include "c4DocumentTypes.h"
#include "c4IndexTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/* NOTE:
    Enumeration-related functions are in c4DocEnumerator.h:
    - c4coll_enumerateChanges
    - c4coll_enumerateAllDocs
    Observer-related functions are in c4Observer.h:
    - c4dbobs_createOnCollection
    - c4docobs_createWithCollection
*/


/** \defgroup Collection Collections
    @{
    A `C4Collection` represents a **Collection**, a named grouping of documents in a database.
    You can think of them as "folders" or "directories" for documents, or as like tables.

    Each Collection provides:
    - a namespace for documents (a "docID" is only unique within its Collection)
    - a queryable container, named in `FROM` and `JOIN` clauses.
    - a scope for indexes
    - a scope for document enumerators
    - independent sequence numbers

    Every database starts with a **default Collection**, whose name is `_default`. If the database
    was created by an earlier version of LiteCore, all existing documents will be in the default
    Collection.

    Pre-existing functions that refer to documents / sequences / indexes without referring to
    Collections -- such as \ref c4doc_get and \ref c4db_getLastSequence -- still exist, but implicitly
    operate on the default Collection. In other words, they behave exactly the way they used to,
    but Collection-aware code should avoid them and use the new Collection API instead.
    These functions will eventually be deprecated, then removed.

    > **NOTE:** A few Collection functions are documented in other sections of the API docs:

    - Enumeration-related functions (in `c4DocEnumerator.h`):
      - \ref c4coll_enumerateChanges
      - \ref c4coll_enumerateAllDocs
    - Observer-related functions (in `c4Observer.h`):
      - \ref c4dbobs_createOnCollection
      - \ref c4docobs_createWithCollection
 */


/** \name Lifecycle
    @{ */


/** Returns the default collection that exists in every database.
    In a pre-existing database, this collection contains all docs that were added to
    "the database" before collections existed.
    Its name is "_default". */
C4Collection* c4db_getDefaultCollection(C4Database *db) C4API;

/** Returns true if the collection exists. */
bool c4db_hasCollection(C4Database *db,
                        C4String name) C4API;

/** Returns the existing collection with the given name, or NULL if it doesn't exist. */
C4Collection* C4NULLABLE c4db_getCollection(C4Database *db,
                                            C4String name) C4API;

/** Creates and returns an empty collection with the given name,
    or returns an existing collection by that name. */
C4Collection* c4db_createCollection(C4Database *db,
                                    C4String name,
                                    C4Error* C4NULLABLE outError) C4API;

/** Deletes the collection with the given name. */
bool c4db_deleteCollection(C4Database *db,
                           C4String name,
                           C4Error* C4NULLABLE outError) C4API;

/** Returns the names of all existing collections, in the order in which they were created.
    The result is a string containing the names separated by comma (',') characters. */
C4StringResult c4db_collectionNames(C4Database *db) C4API;


/** @} */
/** \name Accessors
    @{ */


/** Returns the name of the collection. */
C4String c4coll_getName(C4Collection*) C4API;

/** Returns the database containing this collection. */
C4Database* c4coll_getDatabase(C4Collection*) C4API;

/** Returns the number of (undeleted) documents in the collection. */
uint64_t c4coll_getDocumentCount(C4Collection*) C4API;

/** Returns the latest sequence number allocated to a revision. */
C4SequenceNumber c4coll_getLastSequence(C4Collection*) C4API;


/** @} */
/** \name Documents
    @{ */


/** Gets a document from the collection given its ID.
    The current revision is selected (if the document exists.)
    You must call \ref c4doc_release when finished with the document.
    @param collection  The collection to read from.
    @param docID  The document's ID.
    @param mustExist  Governs behavior if no document with that ID exists. If true, the call fails
                        with error kC4NotFound. If false, a C4Document with no contents is returned.
    @param content  How much content to retrieve: metadata only, current revision, or all revisions.
    @param outError  On failure, error information is stored here.
    @return  A new C4Document instance (which must be released), or NULL. */
C4Document* c4coll_getDoc(C4Collection *collection,
                          C4String docID,
                          bool mustExist,
                          C4DocContentLevel content,
                          C4Error* C4NULLABLE outError) C4API;

/** Gets a document from the collection given its sequence number.
 You must call `c4doc_release()` when finished with the document.  */
C4Document* c4coll_getDocBySequence(C4Collection *collection,
                                    C4SequenceNumber,
                                    C4Error* C4NULLABLE outError) C4API;

/** A high-level Put operation, to insert a new or downloaded revision.
    - If `request->existingRevision` is true, then request->history must contain the revision's
      history, with the revision's ID as the first item.
    - Otherwise, a new revision will be created and assigned a revID. The parent revision ID,
      if any, should be given as the single item of request->history.
    Either way, on success the document is returned with the inserted revision selected.
    Note that actually saving the document back to the database is optional -- it only happens
    if request->save is true. You can set this to false if you want to review the changes
    before saving, e.g. to run them through a validation function. */
C4Document* c4coll_putDoc(C4Collection *collection,
                          const C4DocPutRequest *request,
                          size_t * C4NULLABLE outCommonAncestorIndex,
                          C4Error* C4NULLABLE outError) C4API;

/** Convenience function to create a new document. This just a wrapper around \ref c4coll_putDoc.
    If the document already exists, it will fail with the error `kC4ErrorConflict`.
    @param collection  The collection to create the document in
    @param docID  Document ID to create; if null, a UUID will be generated
    @param body  Body of the document
    @param revisionFlags  The flags of the new revision
    @param error Information about any error that occurred
    @return  On success, a new C4Document with the new revision selected; else NULL. */
C4Document* c4coll_createDoc(C4Collection *collection,
                             C4String docID,
                             C4Slice body,
                             C4RevisionFlags revisionFlags,
                             C4Error* C4NULLABLE error) C4API;

/** Moves a document to another collection, possibly with a different ID.
    @param collection  The document's original collection.
    @param docID  The ID of the document to move.
    @param toCollection  The collection to move to.
    @param newDocID  The docID in the new collection, or a NULL slice to keep the original ID.
    @param error Information about any error that occurred
    @return  True on success, false on failure. */
bool c4coll_moveDoc(C4Collection *collection,
                    C4String docID,
                    C4Collection *toCollection,
                    C4String newDocID,
                    C4Error* C4NULLABLE error) C4API;


//////// PURGING & EXPIRATION:


/** @} */
/** \name Purging and Expiration
    @{ */


/** Removes all trace of a document and its revisions from the collection. */
bool c4coll_purgeDoc(C4Collection *collection,
                     C4String docID,
                     C4Error* C4NULLABLE outError) C4API;


/** Sets an expiration date on a document.  After this time the
    document will be purged from the database.
    @param collection The collection to set the expiration date in
    @param docID The ID of the document to set the expiration date for
    @param timestamp The timestamp of the expiration date, in milliseconds since 1/1/1970.
                A value of 0 indicates that the expiration should be cancelled.
    @param outError Information about any error that occurred
    @return true on sucess, false on failure */
bool c4coll_setDocExpiration(C4Collection *collection,
                             C4String docID,
                             C4Timestamp timestamp,
                             C4Error* C4NULLABLE outError) C4API;

/** Returns the expiration time of a document, if one has been set, else 0.
    @param collection  The collection to set the expiration date in
    @param docID  The ID of the document to check
    @param outError Information about any error that occurred
    @return The timestamp of the expiration date, in milliseconds since 1/1/1970,
                or 0 if the document does not expire,
                or -1 if an error occurred. */
C4Timestamp c4coll_getDocExpiration(C4Collection *collection,
                                    C4String docID,
                                    C4Error* C4NULLABLE outError) C4API;

/** Returns the time at which the next document expiration in this collection should take place,
    or 0 if there are no documents with expiration times. */
C4Timestamp c4coll_nextDocExpiration(C4Collection *) C4API;

/** Purges all documents that have expired.
    @return  The number of documents purged, or -1 on error. */
int64_t c4coll_purgeExpiredDocs(C4Collection *,
                                C4Error* C4NULLABLE) C4API;


/** @} */
/** \name Indexes
    @{ */


/** Creates a collection index, of the values of specific expressions across all documents.
    The name is used to identify the index for later updating or deletion; if an index with the
    same name already exists, it will be replaced unless it has the exact same expressions.

    @param collection  The collection to index.
    @param name  The name of the index. Any existing index with the same name will be replaced,
                 unless it has the identical expressions (in which case this is a no-op.)
    @param indexSpec  The definition of the index in JSON or N1QL form. (See above.)
    @param queryLanguage  The language of `indexSpec`, either JSON or N1QL.
    @param indexType  The type of index (value or full-text.)
    @param indexOptions  Options for the index. If NULL, each option will get a default value.
    @param outError  On failure, will be set to the error status.
    @return  True on success, false on failure. */
bool c4coll_createIndex(C4Collection *collection,
                        C4String name,
                        C4String indexSpec,
                        C4QueryLanguage queryLanguage,
                        C4IndexType indexType,
                        const C4IndexOptions* C4NULLABLE indexOptions,
                        C4Error* C4NULLABLE outError) C4API;

/** Deletes an index that was created by `c4coll_createIndex`.
    @param collection  The collection to index.
    @param name The name of the index to delete
    @param outError  On failure, will be set to the error status.
    @return  True on success, false on failure. */
bool c4coll_deleteIndex(C4Collection *collection,
                        C4String name,
                        C4Error* C4NULLABLE outError) C4API;

/** Returns information about all indexes in the collection.
    The result is a Fleece-encoded array of dictionaries, one per index.
    Each dictionary has keys `"name"`, `"type"` (a `C4IndexType`), and `"expr"` (the source expression).
    @param collection  The collection to check
    @param outError  On failure, will be set to the error status.
    @return  A Fleece-encoded array of dictionaries, or NULL on failure. */
C4SliceResult c4coll_getIndexesInfo(C4Collection* collection,
                                    C4Error* C4NULLABLE outError) C4API;

/** @} */
/** @} */   // end Collections group


C4API_END_DECLS
C4_ASSUME_NONNULL_END
