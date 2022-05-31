//
//  c4Collection.h
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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


/** \defgroup Collection Collections and Scopes
    @{
    A `C4Collection` represents a **Collection**, a named grouping of documents in a database.
    You can think of them as "folders" or "directories" for documents, or as SQL tables.

    Each Collection provides:
    - a namespace for documents (a "docID" is only unique within its Collection)
    - a queryable container, named in `FROM` and `JOIN` clauses.
    - a scope for indexes
    - a scope for document enumerators
    - independent sequence numbers

    Likewise, a **Scope** is a grouping of Collections, like a "parent folder".

    Every database starts with a **default Collection**, whose name is `_default`, which exists in
    a **default Scope**, also named `_default`. If the database was created by an earlier version
    of LiteCore, all existing documents will be in the default Collection.

    ## Collection Naming
    In this API, collections are named by a \ref C4CollectionSpec struct, which simply contains two
    `FLString`s, first a collection name and second a scope name. Note that the collection name
    comes first (unlike in a N1QL query), so that the scope name can be left out if you're
    referring to the default scope. You can give a collection spec literally as e.g.
    `{C4STR("mycoll")}`, or with a scope, `{C4STR("mycoll"), C4STR("myscope")}`.

    There are no API calls to create or delete Scopes. A Scope is created implicitly when you create
    the first Collection inside it, and deleted implicitly when you delete its last Collection.

    ## Legacy `C4Database` Functions
    Pre-existing functions that refer to documents / sequences / indexes without referring to
    Collections -- such as \ref c4doc_get and \ref c4db_getLastSequence -- still exist, but implicitly
    operate on the default Collection. In other words, they behave exactly the way they used to,
    but Collection-aware code should avoid them and use the new Collection API instead.

    These functions will eventually be deprecated, then removed. As an aid in updating your code,
    you can define the C preprocessor symbol `C4_STRICT_COLLECTION_API` to suppress the definitions
    of those functions, which will turn all calls to them into errors.

    ## `C4Collection` Lifespan
     `C4Collection` is ref-counted, but most of the time you don't need to retain or release it.
    The `C4Database` owns its collections, so a `C4Collection` reference remains valid until either
    the database is closed, or that collection is deleted. At that point it becomes a dangling
    pointer :( If you keep a collection reference long-term, you should retain it so that the
    reference remains valid until you release it.

    A retained C4Collection _object_ still becomes invalid after it's deleted or its database
    closes. After that, most operations on it will fail (safely), returning \ref kC4ErrorNotOpen
    or some null/zero result. You can tell whether a C4Collection is valid by calling
    \ref c4coll_isValid, or by checking whether \ref c4coll_getDatabase returns non-NULL.

    ## Other Documentation
    A few Collection functions are documented in other sections of the API docs:

    - Enumeration-related functions (in `c4DocEnumerator.h`):
      - \ref c4coll_enumerateChanges
      - \ref c4coll_enumerateAllDocs
    - Observer-related functions (in `c4Observer.h`):
      - \ref c4dbobs_createOnCollection
      - \ref c4docobs_createWithCollection */


/** \name Lifecycle
    @{ */


/** Returns the default collection, whose name is "`_default`" (`kC4DefaultCollectionName`).
    This is the one collection that exists in every newly created database.
    When a pre-existing database is upgraded to support collections, all its documents are put
    in the default collection.
    @note  This function never returns NULL, unless the default collection has been deleted.
           Also be sure to read `C4Collection` Lifespan in c4Collection.h. */
CBL_CORE_API C4Collection* c4db_getDefaultCollection(C4Database *db) C4API;

/** Returns true if the collection exists. */
CBL_CORE_API bool c4db_hasCollection(C4Database *db,
                        C4CollectionSpec spec) C4API;

/** Returns true if the named scope exists.  Note that _default will always return true. */
CBL_CORE_API bool c4db_hasScope(C4Database *db, C4String name) C4API;

/** Returns the existing collection with the given name & scope, or NULL if it doesn't exist. 
    @note Be sure to read `C4Collection` Lifespan in c4Collection.h. */
CBL_CORE_API C4Collection* C4NULLABLE c4db_getCollection(C4Database *db,
                                            C4CollectionSpec spec) C4API;

/** Creates and returns an empty collection with the given name & scope.
    If the collection already exists, it just returns it.
    If the scope doesn't exist, it is implicitly created. 
    @note Be sure to read `C4Collection` Lifespan in c4Collection.h. */
CBL_CORE_API C4Collection* C4NULLABLE c4db_createCollection(C4Database *db,
                                               C4CollectionSpec spec,
                                               C4Error* C4NULLABLE outError) C4API;

/** Deletes the collection with the given name & scope.
    Deleting the last collection from a scope implicitly deletes the scope.
    @note  It is legal to delete the default collection, but it cannot be re-created. */
CBL_CORE_API bool c4db_deleteCollection(C4Database *db,
                           C4CollectionSpec spec,
                           C4Error* C4NULLABLE outError) C4API;

/** Returns the names of all existing collections in the given scope,
    in the order in which they were created.
    @note  You are responsible for releasing the returned Fleece array. */
CBL_CORE_API FLMutableArray c4db_collectionNames(C4Database *db,
                                    C4String inScope) C4API;

/** Returns the names of all existing scopes, in the order in which they were created.
    @note  You are responsible for releasing the returned Fleece array. */
CBL_CORE_API FLMutableArray c4db_scopeNames(C4Database *db) C4API;


/** @} */
/** \name Accessors
    @{ */


/** Returns false if this collection has been deleted, or its database closed. */
CBL_CORE_API bool c4coll_isValid(C4Collection* C4NULLABLE) C4API;

/** Returns the name and scope of the collection. */
CBL_CORE_API C4CollectionSpec c4coll_getSpec(C4Collection*) C4API;

/** Returns the database containing the collection, or NULL if the collection is invalid. */
CBL_CORE_API C4Database* c4coll_getDatabase(C4Collection*) C4API;

/** Returns the number of (undeleted) documents in the collection. */
CBL_CORE_API uint64_t c4coll_getDocumentCount(C4Collection*) C4API;

/** Returns the latest sequence number allocated to a revision. */
CBL_CORE_API C4SequenceNumber c4coll_getLastSequence(C4Collection*) C4API;


/** @} */
/** \name Documents
    @{ */


/** Gets a document from the collection given its ID.
    The current revision is selected (if the document exists.)
    @note  You must call \ref c4doc_release when finished with the document.
    @param collection  The collection to read from.
    @param docID  The document's ID.
    @param mustExist  Governs behavior if no document with that ID exists. If true, the call fails
                        with error kC4NotFound. If false, a C4Document with no contents is returned.
    @param content  How much content to retrieve: metadata only, current revision, or all revisions.
    @param outError  On failure, error information is stored here.
    @return  A new C4Document instance (which must be released), or NULL. */
CBL_CORE_API C4Document* C4NULLABLE c4coll_getDoc(C4Collection *collection,
                                     C4String docID,
                                     bool mustExist,
                                     C4DocContentLevel content,
                                     C4Error* C4NULLABLE outError) C4API;

/** Gets a document from the collection given its sequence number.
    @note  You must call `c4doc_release()` when finished with the document.  */
C4Document* C4NULLABLE c4coll_getDocBySequence(C4Collection *collection,
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
    before saving, e.g. to run them through a validation function.
    @note  You must call \ref c4doc_release when finished with the returned document. */
CBL_CORE_API C4Document* C4NULLABLE c4coll_putDoc(C4Collection *collection,
                                     const C4DocPutRequest *request,
                                     size_t * C4NULLABLE outCommonAncestorIndex,
                                     C4Error* C4NULLABLE outError) C4API;

/** Convenience function to create a new document. This just a wrapper around \ref c4coll_putDoc.
    If the document already exists, it will fail with the error `kC4ErrorConflict`.
    @note  You must call \ref c4doc_release when finished with the document.
    @param collection  The collection to create the document in
    @param docID  Document ID to create; if null, a UUID will be generated
    @param body  Body of the document
    @param revisionFlags  The flags of the new revision
    @param error Information about any error that occurred
    @return  On success, a new C4Document with the new revision selected; else NULL. */
CBL_CORE_API C4Document* C4NULLABLE c4coll_createDoc(C4Collection *collection,
                                        C4String docID,
                                        C4Slice body,
                                        C4RevisionFlags revisionFlags,
                                        C4Error* C4NULLABLE error) C4API;

/** Moves a document to another collection, possibly with a different docID.
    @param collection  The document's original collection.
    @param docID  The ID of the document to move.
    @param toCollection  The collection to move to.
    @param newDocID  The docID in the new collection, or a NULL slice to keep the original ID.
    @param error Information about any error that occurred
    @return  True on success, false on failure. */
CBL_CORE_API bool c4coll_moveDoc(C4Collection *collection,
                    C4String docID,
                    C4Collection *toCollection,
                    C4String newDocID,
                    C4Error* C4NULLABLE error) C4API;


//////// PURGING & EXPIRATION:


/** @} */
/** \name Purging and Expiration
    @{ */


/** Removes all trace of a document and its revisions from the collection. */
CBL_CORE_API bool c4coll_purgeDoc(C4Collection *collection,
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
CBL_CORE_API bool c4coll_setDocExpiration(C4Collection *collection,
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
CBL_CORE_API C4Timestamp c4coll_getDocExpiration(C4Collection *collection,
                                    C4String docID,
                                    C4Error* C4NULLABLE outError) C4API;

/** Returns the time at which the next document expiration in this collection should take place,
    or 0 if there are no documents with expiration times. */
CBL_CORE_API C4Timestamp c4coll_nextDocExpiration(C4Collection *) C4API;

/** Purges all documents that have expired.
    @return  The number of documents purged, or -1 on error. */
CBL_CORE_API int64_t c4coll_purgeExpiredDocs(C4Collection *,
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
CBL_CORE_API bool c4coll_createIndex(C4Collection *collection,
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
CBL_CORE_API bool c4coll_deleteIndex(C4Collection *collection,
                        C4String name,
                        C4Error* C4NULLABLE outError) C4API;

/** Returns information about all indexes in the collection.
    The result is a Fleece-encoded array of dictionaries, one per index.
    Each dictionary has keys `"name"`, `"type"` (a `C4IndexType`), and `"expr"` (the source expression).
    @param collection  The collection to check
    @param outError  On failure, will be set to the error status.
    @return  A Fleece-encoded array of dictionaries, or NULL on failure. */
CBL_CORE_API C4SliceResult c4coll_getIndexesInfo(C4Collection* collection,
                                    C4Error* C4NULLABLE outError) C4API;

/** @} */
/** @} */   // end Collections group


C4API_END_DECLS
C4_ASSUME_NONNULL_END
