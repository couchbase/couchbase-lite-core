//
// c4Document.h
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4DocumentTypes.h"

#if LITECORE_CPP_API
#    include "c4Document.hh"  // C++ version of C4Document struct
#else
#    include "c4DocumentStruct.h"  // C version of C4Document struct
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Documents Documents
        @{ */


/** \name Lifecycle
        @{ */


#define kC4GeneratedIDLength 23

/** Generates a random 23-byte C string suitable for use as a unique new document ID.
        @param buffer  Where to write the string.
        @param bufferSize  Size of the buffer (must be at least kC4GeneratedIDLength + 1)
        @return  A pointer to the string in the buffer, or NULL if the buffer is too small. */
CBL_CORE_API char* c4doc_generateID(char* buffer, size_t bufferSize) C4API;


#ifndef C4_STRICT_COLLECTION_API

/** Gets a document from the database given its ID.
        The current revision is selected (if the document exists.)
        You must call `c4doc_release()` when finished with the document.
        \note The caller must use a lock for Database when this function is called.
        @param database  The database to read from.
        @param docID  The document's ID.
        @param mustExist  Governs behavior if no document with that ID exists. If true, the call fails
                            with error kC4NotFound. If false, a C4Document with no contents is returned.
        @param content  How much content to retrieve: metadata only, current revision, or all revisions.
        @param outError  On failure, error information is stored here.
        @return  A new C4Document instance (which must be released), or NULL. */
NODISCARD CBL_CORE_API C4Document* c4db_getDoc(C4Database* database, C4String docID, bool mustExist,
                                               C4DocContentLevel content, C4Error* C4NULLABLE outError) C4API;

/** Gets a document from the database given its ID (semi-deprecated).
        This is the same as \ref c4db_getDoc with `content` equal to `kDocGetCurrentRev`. 
        \note The caller must use a lock for Database when this function is called. */
NODISCARD CBL_CORE_API C4Document* c4doc_get(C4Database* database, C4String docID, bool mustExist,
                                             C4Error* C4NULLABLE outError) C4API;

/** Gets a document from the database given its sequence number.
        You must call `c4doc_release()` when finished with the document.
        \note The caller must use a lock for Database when this function is called. */
NODISCARD CBL_CORE_API C4Document* c4doc_getBySequence(C4Database*         database, C4SequenceNumber,
                                                       C4Error* C4NULLABLE outError) C4API;

#endif

/** Saves changes to a C4Document.
        Must be called within a transaction.
        The revision history will be pruned to the maximum depth given.
        \note The caller must use a lock for Database and Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_save(C4Document* doc, uint32_t maxRevTreeDepth, C4Error* C4NULLABLE outError) C4API;

/** @} */


//////// REVISIONS:


/** \name Revisions
        @{ */

/*** Returns whether the selected revision has been rejected by the remote
    \note The caller must use a lock for Document when this function is called. */
CBL_CORE_API bool c4doc_isRevRejected(C4Document* doc) C4API;

/** Selects a specific revision of a document (or no revision, if revID is NULL.) 
    \note The caller must use a lock for Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_selectRevision(C4Document* doc, C4String revID, bool withBody,
                                                 C4Error* C4NULLABLE outError) C4API;

/** Selects the current revision of a document.
        (This is the first revision, in the order they appear in the document.) 
        \note The caller must use a lock for Document when this function is called. */
CBL_CORE_API bool c4doc_selectCurrentRevision(C4Document* doc) C4API;

/** Populates the body field of a doc's selected revision,
        if it was initially loaded without its body. 
        \note The caller must use a lock for Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_loadRevisionBody(C4Document* doc, C4Error* C4NULLABLE outError) C4API;

/** Returns true if the body of the selected revision is available,
        i.e. if c4doc_loadRevisionBody() would succeed.
        \note The caller must use a lock for Document when this function is called. */
CBL_CORE_API bool c4doc_hasRevisionBody(C4Document* doc) C4API;

/** Returns the body (encoded Fleece data) of the selected revision, if available.
        \note The caller must use a lock for Document when this function is called.
        \warning  In a version-vector document, and if this is not the current revision,
                  the returned slice is invalidated the next time this function is called. */
CBL_CORE_API C4Slice c4doc_getRevisionBody(C4Document* doc) C4API;

/** Returns a string encoding the selected revision's history, as comma-separate revision/version IDs
        in reverse chronological order.
        In a version-vector database this is of course the revision's version vector. It will be in
        global form (real SourceID instead of "*") unless the `maxRevs` parameter is 0.
        \note The caller must use a lock for Document when this function is called.
        @param doc  The document.
        @param maxRevs  The maximum number of revisions to include in the result; or 0 for unlimited.
        @param backToRevs  An array of revision IDs: the history should stop when it gets to any of
                            these, and _must_ go back to one of these if possible, even if it means
                            skipping some revisions.
        @param backToRevsCount  The number of revisions in the `backToRevs` array.
        @return  A string of comma-separate revision/version IDs in reverse chronological order. */
CBL_CORE_API C4SliceResult c4doc_getRevisionHistory(C4Document* doc, unsigned maxRevs,
                                                    const C4String backToRevs[C4NULLABLE],
                                                    unsigned       backToRevsCount) C4API;

/** Returns the selected revision's ID in a form that will make sense to another peer/server.
        (This doesn't affect tree-based revIDs. In vector-based version IDs it uses the database's actual
        peer ID instead of the shorthand "*" character.) 
        \note The caller must use a lock for Document when this function is called. */
CBL_CORE_API C4SliceResult c4doc_getSelectedRevIDGlobalForm(C4Document* doc) C4API;

/** Selects the parent of the selected revision, if it's known, else returns false.
        \note The caller must use a lock for Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_selectParentRevision(C4Document* doc) C4API;

/** Selects the next revision in priority order.
        This can be used to iterate over all revisions, starting from the current revision.
        \note The caller must use a lock for Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_selectNextRevision(C4Document* doc) C4API;

/** Selects the next leaf revision; like selectNextRevision but skips over non-leaves.
        To distinguish between the end of the iteration and a failure, check the value of
        *outError after the function returns false: if there's no error (code==0) it's normal.
        \note The caller must use a lock for Documnet when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_selectNextLeafRevision(C4Document* doc, bool includeDeleted, bool withBody,
                                                         C4Error* C4NULLABLE outError) C4API;

/** Selects the common ancestor of two revisions. Returns false if none is found.
        \note The caller must use a lock for Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_selectCommonAncestorRevision(C4Document* doc, C4String rev1ID, C4String rev2ID) C4API;

/** Looks up or creates a numeric ID identifying a remote database, for use with
        c4doc_getRemoteAncestor() and c4doc_setRemoteAncestor().
        \note The caller must use a lock for Database when this function is called.
        @param db  The database.
        @param remoteAddress  The replication URL of the remote db, or its other unique identifier.
        @param canCreate  If true, a new identifier will be created if one doesn't exist.
        @param outError  Error information is stored here.
        @return  The ID, or 0 on error. */
CBL_CORE_API C4RemoteID c4db_getRemoteDBID(C4Database* db, C4String remoteAddress, bool canCreate,
                                           C4Error* C4NULLABLE outError) C4API;

/** Given a remote database ID, returns its replication URL / unique identifier.
        \note The caller must use a lock for Database when this function is called.
        @param db  The database.
        @param remoteID  The ID assigned to the remote database.
        @return  The URL/identifier, or a null slice if not found. */
CBL_CORE_API C4SliceResult c4db_getRemoteDBAddress(C4Database* db, C4RemoteID remoteID) C4API;

/** Returns the revision ID that has been marked as current for the given remote database.
        \note The caller must use a lock for Database and Document when this function is called. */
CBL_CORE_API C4SliceResult c4doc_getRemoteAncestor(C4Document* doc, C4RemoteID remoteDatabase) C4API;

/** Marks a revision as current for the given remote database.
        \note The caller must use a lock for Database and Document when this function is called. */
NODISCARD CBL_CORE_API bool c4doc_setRemoteAncestor(C4Document* doc, C4RemoteID remoteDatabase, C4String revID,
                                                    C4Error* C4NULLABLE error) C4API;

/** Given a tree-based revision ID, returns its generation number (the decimal number before
        the hyphen), or zero if it's unparseable.
    @warning This function does not support version-based revision IDs. Given one it returns zero,
    because the timestamp would be too big to return on platforms where `unsigned` is 32-bit.
    Use \ref c4rev_getTimestamp to support version-based revIDs. */
CBL_CORE_API unsigned c4rev_getGeneration(C4String revID) C4API;

/** Given a revision ID that's a Version (of the form `time@peer`), returns its timestamp.
    This can be interpreted as the time the revision was created, in nanoseconds since the Unix
    epoch, but it's not necessarily exact.

    If this is a tree-based revision ID (of the form `gen-digest`), it returns the generation
    number. (This can be distinguished from a timestamp because it's much, much smaller!
    Timestamps will be at least 2^50, while generations rarely hit one million.)*/
CBL_CORE_API uint64_t c4rev_getTimestamp(C4String revID) C4API;


/** Returns true if two revision IDs are equivalent.
        - Digest-based IDs are equivalent only if byte-for-byte equal.
        - Version-vector based IDs are equivalent if their initial versions are equal. */
CBL_CORE_API bool c4rev_equal(C4Slice rev1, C4Slice rev2) C4API;


/** Removes a branch from a document's history. The revID must correspond to a leaf
        revision; that revision and its ancestors will be removed, except for ancestors that are
        shared with another branch.
        If the document has only one branch (no conflicts), or if the input revID is null, the
        purge will remove every revision, and saving the document will purge it (remove it
        completely from the database.)
        Must be called within a transaction. Remember to save the document afterwards.
        \note The caller must use a lock for Document when this function is called.
        @param doc  The document.
        @param revID  The ID of the revision to purge. If null, all revisions are purged.
        @param outError  Error information is stored here.
        @return  The total number of revisions purged (including ancestors), or -1 on error. */
NODISCARD CBL_CORE_API int32_t c4doc_purgeRevision(C4Document* doc, C4String revID, C4Error* C4NULLABLE outError) C4API;

/** Resolves a conflict between two leaf revisions, by deleting one of them and optionally
        adding a new merged revision as a child of the other.
        Must be called within a transaction. Remember to save the document afterwards.
        \note The caller must use a lock for Document when this function is called.
        @param doc  The document.
        @param winningRevID  The conflicting revision to be kept (and optionally updated.)
        @param losingRevID  The conflicting revision to be deleted.
        @param mergedBody  The body of the merged revision, or NULL if none.
        @param mergedFlags  Flags for the merged revision.
        @param error  Error information is stored here.
        @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4doc_resolveConflict(C4Document* doc, C4String winningRevID, C4String losingRevID,
                                                  C4Slice mergedBody, C4RevisionFlags mergedFlags,
                                                  C4Error* C4NULLABLE error) C4API;

/** @} */


//////// PURGING & EXPIRATION:


#ifndef C4_STRICT_COLLECTION_API

/** \name Purging and Expiration
        @{ */


/** Removes all trace of a document and its revisions from the database. 
    \note The caller must use a lock for Database when this function is called. */
CBL_CORE_API bool c4db_purgeDoc(C4Database* database, C4String docID, C4Error* C4NULLABLE outError) C4API;


/** Sets an expiration date on a document.  After this time the
        document will be purged from the database.
        \note The caller must use a lock for Database when this function is called.
        @param db The database to set the expiration date in
        @param docID The ID of the document to set the expiration date for
        @param timestamp The timestamp of the expiration date, in milliseconds since 1/1/1970.
                    A value of 0 indicates that the expiration should be cancelled.
        @param outError Information about any error that occurred
        @return true on sucess, false on failure */
CBL_CORE_API bool c4doc_setExpiration(C4Database* db, C4String docID, C4Timestamp timestamp,
                                      C4Error* C4NULLABLE outError) C4API;

/** Returns the expiration time of a document, if one has been set, else 0.
        \note The caller must use a lock for Database when this function is called.
        @param db  The database to set the expiration date in
        @param docID  The ID of the document to check
        @param outError Information about any error that occurred
        @return The timestamp of the expiration date, in milliseconds since 1/1/1970,
                    or 0 if the document does not expire,
                    or -1 if an error occurred. */
CBL_CORE_API C4Timestamp c4doc_getExpiration(C4Database* db, C4String docID, C4Error* C4NULLABLE outError) C4API;

#endif  // C4_STRICT_COLLECTION_API

/** @} */


//////// ADDING REVISIONS:


/** \name Creating and Updating Documents
        @{ */

#ifndef C4_STRICT_COLLECTION_API

/** A high-level Put operation, to insert a new or downloaded revision.
        * If request->existingRevision is true, then request->history must contain the revision's
          history, with the revision's ID as the first item.
        * Otherwise, a new revision will be created and assigned a revID. The parent revision ID,
          if any, should be given as the single item of request->history.
        Either way, on success the document is returned with the inserted revision selected.
        Note that actually saving the document back to the database is optional -- it only happens
        if request->save is true. You can set this to false if you want to review the changes
        before saving, e.g. to run them through a validation function.
        \note The caller must use a lock for Database when this function is called. */
CBL_CORE_API C4Document* c4doc_put(C4Database* database, const C4DocPutRequest* request,
                                   size_t* C4NULLABLE outCommonAncestorIndex, C4Error* C4NULLABLE outError) C4API;

/** Convenience function to create a new document. This just a wrapper around c4doc_put.
        If the document already exists, it will fail with the error kC4ErrorConflict.
        \note The caller must use a lock for Database when this function is called.
        @param db  The database to create the document in
        @param docID  Document ID to create; if null, a UUID will be generated
        @param body  Body of the document
        @param revisionFlags  The flags of the new revision
        @param error Information about any error that occurred
        @return  On success, a new C4Document with the new revision selected; else NULL. */
CBL_CORE_API C4Document* c4doc_create(C4Database* db, C4String docID, C4Slice body, C4RevisionFlags revisionFlags,
                                      C4Error* C4NULLABLE error) C4API;

#endif  // C4_STRICT_COLLECTION_API

/** Adds a revision to a document already in memory as a C4Document. This is more efficient
        than c4doc_put because it doesn't have to read from the database before writing; but if
        the C4Document doesn't have the current state of the document, it will fail with the error
        kC4ErrorConflict -- then you'll need to get the current document and try again.
        The new revision is added as a child of the currently selected revision.
        \note The caller must use a lock for Database and Document when this function is called.
        @param doc  The document to update
        @param revisionBody  The body of the new revision
        @param revisionFlags  The flags of the new revision
        @param error Information about any error that occurred
        @return  On success, a new C4Document with the new revision selected; else NULL. */
CBL_CORE_API C4Document* c4doc_update(C4Document* doc, C4Slice revisionBody, C4RevisionFlags revisionFlags,
                                      C4Error* C4NULLABLE error) C4API;

/** @} */
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
