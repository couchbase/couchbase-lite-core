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
 *  Throws if the document is a version-vectors document.
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

/** Given a revision ID that's a Version (of the form `time\@peer`), returns its timestamp.
    This can be interpreted as the time the revision was created, in nanoseconds since the Unix
    epoch, but it's not necessarily exact.

    If this is a tree-based revision ID (of the form `gen-digest`), it returns the generation
    number. (This can be distinguished from a timestamp because it's much, much smaller!
    Timestamps will be at least 2^50, while generations rarely hit one million.)*/
CBL_CORE_API uint64_t c4rev_getTimestamp(C4String revID) C4API;

/** Given a "legacy" tree-based revision ID, converts it to a synthetic version-based ID
 *  using the standard algorithm (generation and 40 bits of the digest are stuffed into the
 *  Version's timestamp, and the Version's sourceID is a well-known constant.)
 *
 *  Given a version-based revision ID, returns it unchanged. */
CBL_CORE_API C4SliceResult c4rev_legacyAsVersion(C4String revID) C4API;


/** Returns true if two revision IDs are equivalent:
 *  - If both are version vectors (or single versions) and their leading versions are equal;
 *  - or if both are digest-based and are bitwise equal;
 *  - or if one is a digest and converting it to a legacy Version equals the other. */
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
        @param mergedBody  The body of the merged revision, or nullslice to keep the winning rev's body.
        @param mergedFlags  Flags for the merged revision. Ignored if mergedBody is nullslice.
        @param error  Error information is stored here.
        @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4doc_resolveConflict(C4Document* doc, C4String winningRevID, C4String losingRevID,
                                                  C4Slice mergedBody, C4RevisionFlags mergedFlags,
                                                  C4Error* C4NULLABLE error) C4API;

/** @} */

//////// ADDING REVISIONS:


/** \name Creating and Updating Documents
        @{ */

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

C4API_END_DECLS
C4_ASSUME_NONNULL_END
