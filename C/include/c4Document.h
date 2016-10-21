//
//  c4Document.h
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
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
        kDeleted        = 0x01,     ///< The document's current revision is deleted.
        kConflicted     = 0x02,     ///< The document is in conflict.
        kHasAttachments = 0x04,     ///< The document's current revision has attachments.

        kExists         = 0x1000    ///< The document exists (i.e. has revisions.)
    }; // Note: Superset of VersionedDocument::Flags

    /** Flags that apply to a revision. */
    typedef C4_OPTIONS(uint8_t, C4RevisionFlags) {
        kRevDeleted        = 0x01, ///< Is this revision a deletion/tombstone?
        kRevLeaf           = 0x02, ///< Is this revision a leaf (no children?)
        kRevNew            = 0x04, ///< Has this rev been inserted since the doc was read?
        kRevHasAttachments = 0x08  ///< Does this rev's body contain attachments?
    }; // Note: Same as Revision::Flags


    /** Describes a revision of a document. A sub-struct of C4Document. */
    typedef struct {
        C4Slice revID;              ///< Revision ID
        C4RevisionFlags flags;      ///< Flags (deleted?, leaf?, new? hasAttachments?)
        C4SequenceNumber sequence;  ///< Sequence number in database
        C4Slice body;               ///< The raw body, or NULL if not loaded yet
    } C4Revision;


    /** Describes a version-controlled document. */
    typedef struct C4Document {
        C4DocumentFlags flags;      ///< Document flags
        C4Slice docID;              ///< Document ID
        C4Slice revID;              ///< Revision ID of current revision
        C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

        C4Revision selectedRev;     ///< Describes the currently-selected revision
    } C4Document;


    /** \name Lifecycle
        @{ */


    /** Gets a document from the database. If there's no such document, the behavior depends on
        the mustExist flag. If it's true, NULL is returned. If it's false, a valid but empty
        C4Document is returned, that doesn't yet exist in the database (but will be added when
        saved.)
        The current revision is selected (if the document exists.) */
    C4Document* c4doc_get(C4Database *database,
                          C4Slice docID,
                          bool mustExist,
                          C4Error *outError) C4API;

    /** Gets a document from the database given its sequence number. */
    C4Document* c4doc_getBySequence(C4Database *database,
                                    C4SequenceNumber,
                                    C4Error *outError) C4API;

    /** Returns the document type (as set by setDocType.) This value is ignored by LiteCore itself;
        by convention Couchbase Lite sets it to the value of the current revision's "type" property,
        and uses it as an optimization when indexing a view. */
    C4SliceResult c4doc_getType(C4Document *doc) C4API;

    /** Sets a document's docType. (By convention this is the value of the "type" property of the
        current revision's JSON; this value can be used as optimization when indexing a view.)
        The change will not be persisted until the document is saved. */
    void c4doc_setType(C4Document *doc, C4Slice docType) C4API;

    /** Saves changes to a C4Document.
        Must be called within a transaction.
        The revision history will be pruned to the maximum depth given. */
    bool c4doc_save(C4Document *doc,
                    uint32_t maxRevTreeDepth,
                    C4Error *outError) C4API;

    /** Frees a C4Document. */
    void c4doc_free(C4Document *doc) C4API;

    /** @} */
    

    //////// REVISIONS:


    /** \name Revisions
        @{ */


    /** Selects a specific revision of a document (or no revision, if revID is NULL.) */
    bool c4doc_selectRevision(C4Document* doc,
                              C4Slice revID,
                              bool withBody,
                              C4Error *outError) C4API;

    /** Selects the current revision of a document.
        (This is the first revision, in the order they appear in the document.) */
    bool c4doc_selectCurrentRevision(C4Document* doc) C4API;

    /** Populates the body field of a doc's selected revision,
        if it was initially loaded without its body. */
    bool c4doc_loadRevisionBody(C4Document* doc,
                                C4Error *outError) C4API;

    /** Transfers ownership of the document's `selectedRev.body` to the caller, without copying.
        The C4Document's field is cleared, and the value returned from this function. As witt
        all C4SliceResult values, the caller is responsible for freeing it when finished. */
    C4SliceResult c4doc_detachRevisionBody(C4Document* doc) C4API;

    /** Returns true if the body of the selected revision is available,
        i.e. if c4doc_loadRevisionBody() would succeed. */
    bool c4doc_hasRevisionBody(C4Document* doc) C4API;

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
                                      C4Error *outError) C4API;

    /** Given a revision ID, returns its generation number (the decimal number before
        the hyphen), or zero if it's unparseable. */
    unsigned c4rev_getGeneration(C4Slice revID) C4API;


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
                                    C4Slice revID,
                                    C4Error *outError) C4API;

    /** @} */

    
    //////// PURGING & EXPIRATION:
        

    /** \name Purging and Expiration
        @{ */


    /** Removes all trace of a document and its revisions from the database. */
    bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) C4API;


    /** Sets an expiration date on a document.  After this time the
        document will be purged from the database.
        @param db The database to set the expiration date in
        @param docId The ID of the document to set the expiration date for
        @param timestamp The UNIX timestamp of the expiration date (must
                    be in the future, i.e. after the current value of time()).  A value
                    of 0 indicates that the expiration should be cancelled.
        @param outError Information about any error that occurred
        @return true on sucess, false on failure */
    bool c4doc_setExpiration(C4Database *db,
                             C4Slice docId,
                             uint64_t timestamp,
                             C4Error *outError) C4API;

    /** Returns the expiration time of a document, if one has been set, else 0. */
    uint64_t c4doc_getExpiration(C4Database *db, C4Slice docId) C4API;

    /** @} */


    //////// ADDING REVISIONS:


    /** \name Creating and Updating Documents
        @{ */


    /** Parameters for adding a revision using c4doc_put. */
    typedef struct {
        C4Slice body;               ///< Revision's body
        C4Slice docID;              ///< Document ID
        C4Slice docType;            ///< Document type if any (used by indexer)
        bool deletion;              ///< Is this revision a deletion?
        bool hasAttachments;        ///< Does this revision have attachments?
        bool existingRevision;      ///< Is this an already-existing rev coming from replication?
        bool allowConflict;         ///< OK to create a conflict, i.e. can parent be non-leaf?
        const C4Slice *history;     ///< Array of ancestor revision IDs
        size_t historyCount;        ///< Size of history[] array
        bool save;                  ///< Save the document after inserting the revision?
        uint32_t maxRevTreeDepth;   ///< Max depth of revision tree to save (or 0 for default)
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
                          size_t *outCommonAncestorIndex,
                          C4Error *outError) C4API;

    /** Generates the revision ID for a new document revision.
        @param body  The (JSON) body of the revision, exactly as it'll be stored.
        @param parentRevID  The revID of the parent revision, or null if there's none.
        @param deletion  True if this revision is a deletion.
        @result  The new revID. Caller is responsible for freeing its buf. */
    C4SliceResult c4doc_generateRevID(C4Slice body, C4Slice parentRevID, bool deletion) C4API;

    /** Set this to true to make c4doc_generateRevID and c4doc_put create revision IDs that
        are identical to the ones Couchbase Lite 1.0--1.2 would create. These use MD5 digests. */
    void c4doc_generateOldStyleRevID(bool generateOldStyle) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
