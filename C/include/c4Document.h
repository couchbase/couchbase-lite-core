//
// c4Document.h
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

#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Documents Documents
        @{ */


    /** Flags describing a document. */
    typedef C4_OPTIONS(uint32_t, C4DocumentFlags) {
        kDocDeleted         = 0x01,     ///< The document's current revision is deleted.
        kDocConflicted      = 0x02,     ///< The document is in conflict.
        kDocHasAttachments  = 0x04,     ///< The document's current revision has attachments.
        kDocExists          = 0x1000    ///< The document exists (i.e. has revisions.)
    }; // Note: Superset of DocumentFlags

    /** Flags that apply to a revision. */
    typedef C4_OPTIONS(uint8_t, C4RevisionFlags) {
        kRevDeleted        = 0x01, ///< Is this revision a deletion/tombstone?
        kRevLeaf           = 0x02, ///< Is this revision a leaf (no children?)
        kRevNew            = 0x04, ///< Has this rev been inserted since the doc was read?
        kRevHasAttachments = 0x08, ///< Does this rev's body contain attachments?
        kRevKeepBody       = 0x10, ///< Revision's body should not be discarded when non-leaf
        kRevIsConflict     = 0x20, ///< Unresolved conflicting revision; will never be current
        kRevClosed         = 0x40, ///< Rev is the (deleted) end of a closed conflicting branch
        kRevPurged         = 0x80, ///< Revision is purged (this flag is never stored in the db)
    }; // Note: Same as litecore::Rev::Flags


    /** Specifies how much content to retrieve when getting a document. */
    typedef C4_ENUM(uint8_t, C4DocContentLevel) {
        kDocGetMetadata,            ///< Only get revID and flags
        kDocGetCurrentRev,          ///< Get current revision body but not other revisions/remotes
        kDocGetAll,                 ///< Get everything
    }; // Note: Same as litecore::ContentOption


    /** Describes a revision of a document. A sub-struct of C4Document. */
    typedef struct {
        C4HeapString revID;         ///< Revision ID
        C4RevisionFlags flags;      ///< Flags (deleted?, leaf?, new? hasAttachments?)
        C4SequenceNumber sequence;  ///< Sequence number in database
    } C4Revision;


    /** Describes a version-controlled document. */
    struct C4Document {
        C4DocumentFlags flags;      ///< Document flags
        C4HeapString docID;         ///< Document ID
        C4HeapString revID;         ///< Revision ID of current revision
        C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

        C4Revision selectedRev;     ///< Describes the currently-selected revision

        C4ExtraInfo extraInfo;      ///< For client use
    };


    /** Identifies a remote database being replicated with. */
    typedef uint32_t C4RemoteID;


    /** \name Lifecycle
        @{ */


    #define kC4GeneratedIDLength 23

    /** Generates a random 23-byte C string suitable for use as a unique new document ID.
        @param buffer  Where to write the string.
        @param bufferSize  Size of the buffer (must be at least kC4GeneratedIDLength + 1)
        @return  A pointer to the string in the buffer, or NULL if the buffer is too small. */
    char* c4doc_generateID(char *buffer, size_t bufferSize) C4API;


    /** Gets a document from the database given its ID.
        The current revision is selected (if the document exists.)
        You must call `c4doc_release()` when finished with the document.
        @param database  The database to read from.
        @param docID  The document's ID.
        @param mustExist  Governs behavior if no document with that ID exists. If true, the call fails
                            with error kC4NotFound. If false, a C4Document with no contents is returned.
        @param content  How much content to retrieve: metadata only, current revision, or all revisions.
        @param outError  On failure, error information is stored here.
        @return  A new C4Document instance (which must be released), or NULL. */
    C4Document* c4db_getDoc(C4Database *database,
                            C4String docID,
                            bool mustExist,
                            C4DocContentLevel content,
                            C4Error* C4NULLABLE outError) C4API;

    /** Gets a document from the database given its ID (semi-deprecated).
        This is the same as \ref c4db_getDoc with `content` equal to `kDocGetCurrentRev`. */
    C4Document* c4doc_get(C4Database *database,
                          C4String docID,
                          bool mustExist,
                          C4Error* C4NULLABLE outError) C4API;

    /** Gets a document from the database given its sequence number.
        You must call `c4doc_release()` when finished with the document.  */
    C4Document* c4doc_getBySequence(C4Database *database,
                                    C4SequenceNumber,
                                    C4Error* C4NULLABLE outError) C4API;

    /** Saves changes to a C4Document.
        Must be called within a transaction.
        The revision history will be pruned to the maximum depth given. */
    bool c4doc_save(C4Document *doc,
                    uint32_t maxRevTreeDepth,
                    C4Error* C4NULLABLE outError) C4API;

    /** @} */
    

    //////// REVISIONS:


    /** \name Revisions
        @{ */


    /** Selects a specific revision of a document (or no revision, if revID is NULL.) */
    bool c4doc_selectRevision(C4Document* doc,
                              C4String revID,
                              bool withBody,
                              C4Error* C4NULLABLE outError) C4API;

    /** Selects the current revision of a document.
        (This is the first revision, in the order they appear in the document.) */
    bool c4doc_selectCurrentRevision(C4Document* doc) C4API;

    /** Populates the body field of a doc's selected revision,
        if it was initially loaded without its body. */
    bool c4doc_loadRevisionBody(C4Document* doc,
                                C4Error* C4NULLABLE outError) C4API;

    /** Returns true if the body of the selected revision is available,
        i.e. if c4doc_loadRevisionBody() would succeed. */
    bool c4doc_hasRevisionBody(C4Document* doc) C4API;

    /** Returns the body (encoded Fleece data) of the selected revision, if available.
        \warning  In a version-vector document, and if this is not the current revision,
                  the returned slice is invalidated the next time this function is called. */
    C4Slice c4doc_getRevisionBody(C4Document* doc) C4API;

    /** Returns a string encoding the selected revision's history, as comma-separate revision/version IDs
        in reverse chronological order.
        In a version-vector database this is of course the revision's version vector. It will be in
        global form (real peerID instead of "*") unless the `maxRevs` parameter is 0.
        @param maxRevs  The maximum number of revisions to include in the result; or 0 for unlimited.
        @param backToRevs  An array of revision IDs: the history should stop when it gets to any of
                            these, and _must_ go back to one of these if possible, even if it means
                            skipping some revisions.
        @param backToRevsCount  The number of revisions in the `backToRevs` array. */
    C4StringResult c4doc_getRevisionHistory(C4Document* doc,
                                           unsigned maxRevs,
                                           const C4String backToRevs[C4NULLABLE],
                                           unsigned backToRevsCount) C4API;

    /** Returns the selected revision's ID in a form that will make sense to another peer/server.
        (This doesn't affect tree-based revIDs. In vector-based version IDs it uses the database's actual
        peer ID instead of the shorthand "*" character.) */
    C4StringResult c4doc_getSelectedRevIDGlobalForm(C4Document* doc) C4API;

    /** Selects the parent of the selected revision, if it's known, else returns NULL. */
    bool c4doc_selectParentRevision(C4Document* doc) C4API;

    /** Selects the next revision in priority order.
        This can be used to iterate over all revisions, starting from the current revision. */
    bool c4doc_selectNextRevision(C4Document* doc) C4API;

    /** Selects the next leaf revision; like selectNextRevision but skips over non-leaves.
        To distinguish between the end of the iteration and a failure, check the value of
        *outError after the function returns false: if there's no error (code==0) it's normal. */
    bool c4doc_selectNextLeafRevision(C4Document* doc,
                                      bool includeDeleted,
                                      bool withBody,
                                      C4Error* C4NULLABLE outError) C4API;

    /** Selects the common ancestor of two revisions. Returns false if none is found. */
    bool c4doc_selectCommonAncestorRevision(C4Document* doc,
                                            C4String rev1ID,
                                            C4String rev2ID) C4API;

    /** Looks up or creates a numeric ID identifying a remote database, for use with
        c4doc_getRemoteAncestor() and c4doc_setRemoteAncestor().
        @param db  The database.
        @param remoteAddress  The replication URL of the remote db, or its other unique identifier.
        @param canCreate  If true, a new identifier will be created if one doesn't exist.
        @param outError  Error information is stored here.
        @return  The ID, or 0 on error. */
    C4RemoteID c4db_getRemoteDBID(C4Database *db,
                                  C4String remoteAddress,
                                  bool canCreate,
                                  C4Error* C4NULLABLE outError) C4API;

    /** Given a remote database ID, returns its replication URL / unique identifier.
        @param db  The database.
        @param remoteID  The ID assigned to the remote database.
        @return  The URL/identifier, or a null slice if not found. */
    C4StringResult c4db_getRemoteDBAddress(C4Database *db,
                                          C4RemoteID remoteID) C4API;

    /** Returns the revision ID that has been marked as current for the given remote database. */
    C4StringResult c4doc_getRemoteAncestor(C4Document *doc,
                                          C4RemoteID remoteDatabase) C4API;

    /** Marks a revision as current for the given remote database. */
    bool c4doc_setRemoteAncestor(C4Document *doc,
                                 C4RemoteID remoteDatabase,
                                 C4String revID,
                                 C4Error* C4NULLABLE error) C4API;

    /** Given a revision ID, returns its generation number (the decimal number before
        the hyphen), or zero if it's unparseable. */
    unsigned c4rev_getGeneration(C4String revID) C4API;


    /** Returns true if two revision IDs are equivalent.
        - Digest-based IDs are equivalent only if byte-for-byte equal.
        - Version-vector based IDs are equivalent if their initial versions are equal. */
    bool c4rev_equal(C4Slice rev1, C4Slice rev2) C4API;


    /** Removes the body of the selected revision and clears its kKeepBody flag.
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document to operate on.
        @return  True if successful, false if unsuccessful. */
    bool c4doc_removeRevisionBody(C4Document* doc) C4API;

    /** Removes a branch from a document's history. The revID must correspond to a leaf
        revision; that revision and its ancestors will be removed, except for ancestors that are
        shared with another branch.
        If the document has only one branch (no conflicts), or if the input revID is null, the
        purge will remove every revision, and saving the document will purge it (remove it
        completely from the database.)
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document.
        @param revID  The ID of the revision to purge. If null, all revisions are purged.
        @param outError  Error information is stored here.
        @return  The total number of revisions purged (including ancestors), or -1 on error. */
    int32_t c4doc_purgeRevision(C4Document *doc,
                                C4String revID,
                                C4Error* C4NULLABLE outError) C4API;

    /** Resolves a conflict between two leaf revisions, by deleting one of them and optionally
        adding a new merged revision as a child of the other.
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document.
        @param winningRevID  The conflicting revision to be kept (and optionally updated.)
        @param losingRevID  The conflicting revision to be deleted.
        @param mergedBody  The body of the merged revision, or NULL if none.
        @param mergedFlags  Flags for the merged revision.
        @param error  Error information is stored here.
        @return  True on success, false on failure. */
    bool c4doc_resolveConflict(C4Document *doc,
                               C4String winningRevID,
                               C4String losingRevID,
                               C4Slice mergedBody,
                               C4RevisionFlags mergedFlags,
                               C4Error* C4NULLABLE error) C4API;

    /** @} */

    
    //////// PURGING & EXPIRATION:
        

    /** \name Purging and Expiration
        @{ */


    /** Removes all trace of a document and its revisions from the database. */
    bool c4db_purgeDoc(C4Database *database, C4String docID, C4Error* C4NULLABLE outError) C4API;


    /** Sets an expiration date on a document.  After this time the
        document will be purged from the database.
        @param db The database to set the expiration date in
        @param docID The ID of the document to set the expiration date for
        @param timestamp The timestamp of the expiration date, in milliseconds since 1/1/1970.
                    A value of 0 indicates that the expiration should be cancelled.
        @param outError Information about any error that occurred
        @return true on sucess, false on failure */
    bool c4doc_setExpiration(C4Database *db,
                             C4String docID,
                             C4Timestamp timestamp,
                             C4Error* C4NULLABLE outError) C4API;

    /** Returns the expiration time of a document, if one has been set, else 0.
        @param db  The database to set the expiration date in
        @param docID  The ID of the document to check
        @param outError Information about any error that occurred
        @return The timestamp of the expiration date, in milliseconds since 1/1/1970,
                    or 0 if the document does not expire,
                    or -1 if an error occurred. */
    C4Timestamp c4doc_getExpiration(C4Database *db,
                                    C4String docID,
                                    C4Error* C4NULLABLE outError) C4API;


    /** @} */


    //////// ADDING REVISIONS:


    /** \name Creating and Updating Documents
        @{ */


    /** Optional callback to `c4doc_put` that generates the new revision body, based on an earlier
        revision body and the body of the `C4DocPutRequest`. It's intended for use when the new
        revision is specified as a delta.
        @param context  The same value given in the `C4DocPutRequest`'s `deltaCBContext` field.
        @param doc  The document; its selected revision is the one requested in the `deltaSourceRevID`.
        @param delta  The contents of the request's `body` or `allocedBody`.
        @param outError  If the callback fails, store an error here if it's non-NULL.
        @return  The body to store in the new revision, or a null slice on failure. */
    typedef C4SliceResult (*C4DocDeltaApplier)(void *context,
                                               C4Document *doc,
                                               C4Slice delta,
                                               C4Error* C4NULLABLE outError);

    /** Parameters for adding a revision using c4doc_put. */
    typedef struct {
        C4String body;              ///< Revision's body
        C4String docID;             ///< Document ID
        C4RevisionFlags revFlags;   ///< Revision flags (deletion, attachments, keepBody)
        bool existingRevision;      ///< Is this an already-existing rev coming from replication?
        bool allowConflict;         ///< OK to create a conflict, i.e. can parent be non-leaf?
        const C4String *history;     ///< Array of ancestor revision IDs
        size_t historyCount;        ///< Size of history[] array
        bool save;                  ///< Save the document after inserting the revision?
        uint32_t maxRevTreeDepth;   ///< Max depth of revision tree to save (or 0 for default)
        C4RemoteID remoteDBID;      ///< Identifier of remote db this rev's from (or 0 if local)

        C4SliceResult allocedBody;  ///< Set this instead of body if body is heap-allocated

        C4DocDeltaApplier C4NULLABLE deltaCB;  ///< If non-NULL, will be called to generate the actual body
        void* C4NULLABLE deltaCBContext;       ///< Passed to `deltaCB` callback
        C4String deltaSourceRevID;  ///< Source rev for delta (must be valid if deltaCB is given)
    } C4DocPutRequest;

    /** A high-level Put operation, to insert a new or downloaded revision.
        * If request->existingRevision is true, then request->history must contain the revision's
          history, with the revision's ID as the first item.
        * Otherwise, a new revision will be created and assigned a revID. The parent revision ID,
          if any, should be given as the single item of request->history.
        Either way, on success the document is returned with the inserted revision selected.
        Note that actually saving the document back to the database is optional -- it only happens
        if request->save is true. You can set this to false if you want to review the changes
        before saving, e.g. to run them through a validation function. */
    C4Document* c4doc_put(C4Database *database,
                          const C4DocPutRequest *request,
                          size_t * C4NULLABLE outCommonAncestorIndex,
                          C4Error* C4NULLABLE outError) C4API;

    /** Convenience function to create a new document. This just a wrapper around c4doc_put.
        If the document already exists, it will fail with the error kC4ErrorConflict.
        @param db  The database to create the document in
        @param docID  Document ID to create; if null, a UUID will be generated
        @param body  Body of the document
        @param revisionFlags  The flags of the new revision
        @param error Information about any error that occurred
        @return  On success, a new C4Document with the new revision selected; else NULL. */
    C4Document* c4doc_create(C4Database *db,
                             C4String docID,
                             C4Slice body,
                             C4RevisionFlags revisionFlags,
                             C4Error* C4NULLABLE error) C4API;

    /** Adds a revision to a document already in memory as a C4Document. This is more efficient
        than c4doc_put because it doesn't have to read from the database before writing; but if
        the C4Document doesn't have the current state of the document, it will fail with the error
        kC4ErrorConflict -- then you'll need to get the current document and try again.
        The new revision is added as a child of the currently selected revision.
        @param doc  The document to update
        @param revisionBody  The body of the new revision
        @param revisionFlags  The flags of the new revision
        @param error Information about any error that occurred
        @return  On success, a new C4Document with the new revision selected; else NULL. */
    C4Document* c4doc_update(C4Document *doc,
                             C4Slice revisionBody,
                             C4RevisionFlags revisionFlags,
                             C4Error* C4NULLABLE error) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif

C4_ASSUME_NONNULL_END
