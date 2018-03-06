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

#include "c4Database.h"

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
    }; // Note: Same as Revision::Flags


    /** Describes a revision of a document. A sub-struct of C4Document. */
    typedef struct {
        C4String revID;              ///< Revision ID
        C4RevisionFlags flags;      ///< Flags (deleted?, leaf?, new? hasAttachments?)
        C4SequenceNumber sequence;  ///< Sequence number in database
        C4String body;               ///< The raw body, or NULL if not loaded yet
    } C4Revision;


    /** Describes a version-controlled document. */
    typedef struct C4Document {
        C4DocumentFlags flags;      ///< Document flags
        C4String docID;              ///< Document ID
        C4String revID;              ///< Revision ID of current revision
        C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

        C4Revision selectedRev;     ///< Describes the currently-selected revision
    } C4Document;


    /** Identifies a remote database being replicated with. */
    typedef uint32_t C4RemoteID;


    /** \name Lifecycle
        @{ */


    /** Gets a document from the database. If there's no such document, the behavior depends on
        the mustExist flag. If it's true, NULL is returned. If it's false, a valid but empty
        C4Document is returned, that doesn't yet exist in the database (but will be added when
        saved.)
        The current revision is selected (if the document exists.) */
    C4Document* c4doc_get(C4Database *database C4NONNULL,
                          C4String docID,
                          bool mustExist,
                          C4Error *outError) C4API;

    /** Gets a document from the database given its sequence number. */
    C4Document* c4doc_getBySequence(C4Database *database C4NONNULL,
                                    C4SequenceNumber,
                                    C4Error *outError) C4API;

    /** Saves changes to a C4Document.
        Must be called within a transaction.
        The revision history will be pruned to the maximum depth given. */
    bool c4doc_save(C4Document *doc C4NONNULL,
                    uint32_t maxRevTreeDepth,
                    C4Error *outError) C4API;

    /** Frees a C4Document. */
    void c4doc_free(C4Document *doc) C4API;

    /** @} */
    

    //////// REVISIONS:


    /** \name Revisions
        @{ */


    /** Selects a specific revision of a document (or no revision, if revID is NULL.) */
    bool c4doc_selectRevision(C4Document* doc C4NONNULL,
                              C4String revID,
                              bool withBody,
                              C4Error *outError) C4API;

    /** Selects the current revision of a document.
        (This is the first revision, in the order they appear in the document.) */
    bool c4doc_selectCurrentRevision(C4Document* doc C4NONNULL) C4API;

    /** Populates the body field of a doc's selected revision,
        if it was initially loaded without its body. */
    bool c4doc_loadRevisionBody(C4Document* doc C4NONNULL,
                                C4Error *outError) C4API;

    /** Transfers ownership of the document's `selectedRev.body` to the caller, without copying.
        The C4Document's field is cleared, and the value returned from this function. As with
        all C4SliceResult values, the caller is responsible for freeing it when finished. */
    C4StringResult c4doc_detachRevisionBody(C4Document* doc C4NONNULL) C4API;

    /** Returns true if the body of the selected revision is available,
        i.e. if c4doc_loadRevisionBody() would succeed. */
    bool c4doc_hasRevisionBody(C4Document* doc C4NONNULL) C4API;

    /** Selects the parent of the selected revision, if it's known, else returns NULL. */
    bool c4doc_selectParentRevision(C4Document* doc C4NONNULL) C4API;

    /** Selects the next revision in priority order.
        This can be used to iterate over all revisions, starting from the current revision. */
    bool c4doc_selectNextRevision(C4Document* doc C4NONNULL) C4API;

    /** Selects the next leaf revision; like selectNextRevision but skips over non-leaves.
        To distinguish between the end of the iteration and a failure, check the value of
        *outError after the function returns false: if there's no error (code==0) it's normal. */
    bool c4doc_selectNextLeafRevision(C4Document* doc C4NONNULL,
                                      bool includeDeleted,
                                      bool withBody,
                                      C4Error *outError) C4API;

    /** Selects the first revision that could be an ancestor of the given revID, or returns false
        if there is none. */
    bool c4doc_selectFirstPossibleAncestorOf(C4Document* doc C4NONNULL,
                                             C4String revID) C4API;

    /** Selects the next revision (after the selected one) that could be an ancestor of the given
        revID, or returns false if there are no more. */
    bool c4doc_selectNextPossibleAncestorOf(C4Document* doc C4NONNULL,
                                            C4String revID) C4API;

    /** Selects the common ancestor of two revisions. Returns false if none is found. */
    bool c4doc_selectCommonAncestorRevision(C4Document* doc C4NONNULL,
                                            C4String rev1ID,
                                            C4String rev2ID) C4API;

    /** Looks up or creates a numeric ID identifying a remote database, for use with
        c4doc_getRemoteAncestor() and c4doc_setRemoteAncestor().
        @param db  The database.
        @param remoteAddress  The replication URL of the remote db, or its other unique identifier.
        @param canCreate  If true, a new identifier will be created if one doesn't exist.
        @param outError  Error information is stored here.
        @return  The ID, or 0 on error. */
    C4RemoteID c4db_getRemoteDBID(C4Database *db C4NONNULL,
                                  C4String remoteAddress,
                                  bool canCreate,
                                  C4Error *outError) C4API;

    /** Returns the revision ID that has been marked as current for the given remote database. */
    C4SliceResult c4doc_getRemoteAncestor(C4Document *doc C4NONNULL,
                                          C4RemoteID remoteDatabase) C4API;

    /** Marks the selected revision as current for the given remote database. */
    bool c4doc_setRemoteAncestor(C4Document *doc C4NONNULL,
                                 C4RemoteID remoteDatabase,
                                 C4Error *error) C4API;

    /** Given a revision ID, returns its generation number (the decimal number before
        the hyphen), or zero if it's unparseable. */
    unsigned c4rev_getGeneration(C4String revID) C4API;


    /** Removes the body of the selected revision and clears its kKeepBody flag.
        Must be called within a transaction. Remember to save the document afterwards.
        @param doc  The document to operate on.
        @return  True if successful, false if unsuccessful. */
    bool c4doc_removeRevisionBody(C4Document* doc C4NONNULL) C4API;

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
        int32_t c4doc_purgeRevision(C4Document *doc C4NONNULL,
                                    C4String revID,
                                    C4Error *outError) C4API;

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
    bool c4doc_resolveConflict(C4Document *doc C4NONNULL,
                               C4String winningRevID,
                               C4String losingRevID,
                               C4Slice mergedBody,
                               C4RevisionFlags mergedFlags,
                               C4Error *error) C4API;

    /** @} */

    
    //////// PURGING & EXPIRATION:
        

    /** \name Purging and Expiration
        @{ */


    /** Removes all trace of a document and its revisions from the database. */
    bool c4db_purgeDoc(C4Database *database C4NONNULL, C4String docID, C4Error *outError) C4API;


    /** Sets an expiration date on a document.  After this time the
        document will be purged from the database.
        @param db The database to set the expiration date in
        @param docId The ID of the document to set the expiration date for
        @param timestamp The UNIX timestamp of the expiration date (must
                    be in the future, i.e. after the current value of time()).  A value
                    of 0 indicates that the expiration should be cancelled.
        @param outError Information about any error that occurred
        @return true on sucess, false on failure */
    bool c4doc_setExpiration(C4Database *db C4NONNULL,
                             C4String docId,
                             uint64_t timestamp,
                             C4Error *outError) C4API;

    /** Returns the expiration time of a document, if one has been set, else 0. */
    uint64_t c4doc_getExpiration(C4Database *db C4NONNULL, C4String docId) C4API;

    /** @} */


    //////// ADDING REVISIONS:


    /** \name Creating and Updating Documents
        @{ */


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
    C4Document* c4doc_put(C4Database *database C4NONNULL,
                          const C4DocPutRequest *request C4NONNULL,
                          size_t *outCommonAncestorIndex,
                          C4Error *outError) C4API;

    /** Convenience function to create a new document. This just a wrapper around c4doc_put.
        If the document already exists, it will fail with the error kC4ErrorConflict.
        @param db  The database to create the document in
        @param docID  Document ID to create; if null, a UUID will be generated
        @param body  Body of the document
        @param revisionFlags  The flags of the new revision
        @param error Information about any error that occurred
        @return  On success, a new C4Document with the new revision selected; else NULL. */
    C4Document* c4doc_create(C4Database *db C4NONNULL,
                             C4String docID,
                             C4Slice body,
                             C4RevisionFlags revisionFlags,
                             C4Error *error) C4API;

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
    C4Document* c4doc_update(C4Document *doc C4NONNULL,
                             C4Slice revisionBody,
                             C4RevisionFlags revisionFlags,
                             C4Error *error) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
