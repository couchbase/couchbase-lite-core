//
//  c4Document.cc
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4Document.h"
#include "c4Database.h"

#include "Database.hh"
#include "Document.hh"
#include "LogInternal.hh"
#include "VersionedDocument.hh"

using namespace cbforest;


struct C4DocumentInternal : public C4Document {
    C4Database* _db;
    VersionedDocument _versionedDoc;
    const Revision *_selectedRev;
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database),
     _versionedDoc(*_db, docID),
     _selectedRev(NULL)
    {
        init();
    }

    C4DocumentInternal(C4Database *database, const Document &doc)
    :_db(database),
     _versionedDoc(*_db, doc),
     _selectedRev(NULL)
    {
        init();
    }

    void init() {
        docID = _versionedDoc.docID();
        flags = (C4DocumentFlags)_versionedDoc.flags();
        if (_versionedDoc.exists())
            flags = (C4DocumentFlags)(flags | kExists);
        initRevID();
        selectCurrentRevision();
    }

    void initRevID() {
        if (_versionedDoc.revID().size > 0) {
            _revIDBuf = _versionedDoc.revID().expanded();
        } else {
            _revIDBuf = slice::null;
        }
        revID = _revIDBuf;
        sequence = _versionedDoc.sequence();
    }

    bool selectRevision(const Revision *rev, C4Error *outError =NULL) {
        _selectedRev = rev;
        _loadedBody = slice::null;
        if (rev) {
            _selectedRevIDBuf = rev->revID.expanded();
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.flags = (C4RevisionFlags)rev->flags;
            selectedRev.sequence = rev->sequence;
            selectedRev.body = rev->inlineBody();
            return true;
        } else {
            _selectedRevIDBuf = slice::null;
            selectedRev.revID = slice::null;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = slice::null;
            recordHTTPError(404, outError);
            return false;
        }
    }

    bool selectCurrentRevision() {
        if (_versionedDoc.revsAvailable()) {
            return selectRevision(_versionedDoc.currentRevision());
        } else {
            // Doc body (rev tree) isn't available, but we know enough about the current rev:
            _selectedRev = NULL;
            selectedRev.revID = revID;
            selectedRev.sequence = sequence;
            int revFlags = 0;
            if (flags & kExists) {
                revFlags |= kRevLeaf;
                if (flags & kDeleted)
                    revFlags |= kRevDeleted;
                if (flags & kHasAttachments)
                    revFlags |= kRevHasAttachments;
            }
            selectedRev.flags = (C4RevisionFlags)revFlags;
            selectedRev.body = slice::null;
            return true;
        }
    }

    bool revisionsLoaded() const {
        return _versionedDoc.revsAvailable();
    }

    bool loadRevisions(C4Error *outError) {
        if (_versionedDoc.revsAvailable())
            return true;
        try {
            WITH_LOCK(_db);
            _versionedDoc.read();
            _selectedRev = _versionedDoc.currentRevision();
            return true;
        } catchError(outError)
        return false;
    }

    bool loadSelectedRevBody(C4Error *outError) {
        if (!loadRevisions(outError))
            return false;
        if (!_selectedRev)
            return true;
        if (selectedRev.body.buf)
            return true;  // already loaded
        try {
            WITH_LOCK(_db);
            _loadedBody = _selectedRev->readBody();
            selectedRev.body = _loadedBody;
            if (_loadedBody.buf)
                return true;
            recordHTTPError(410, outError); // 410 Gone to denote body that's been compacted away
        } catchError(outError);
        return false;
    }

    void updateMeta() {
        _versionedDoc.updateMeta();
        flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
        initRevID();
    }

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    void save(unsigned maxRevTreeDepth) {
        _versionedDoc.prune(maxRevTreeDepth);
        {
            WITH_LOCK(_db);
            _versionedDoc.save(*_db->transaction());
        }
        sequence = _versionedDoc.sequence();
    }

};

static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}

namespace c4Internal {
    C4Document* newC4Document(C4Database *db, const Document &doc) {
        // Doesn't need to lock since Document is already in memory
        return new C4DocumentInternal(db, doc);
    }

    const VersionedDocument& versionedDocument(C4Document* doc) {
        return internal(doc)->_versionedDoc;
    }
}



void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = new C4DocumentInternal(database, docID);
        if (mustExist && !doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = new C4DocumentInternal(database, database->get(sequence));
        if (!doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    try {
        auto idoc = internal(doc);
        if (revID.buf) {
            if (!idoc->loadRevisions(outError))
                return false;
            const Revision *rev = idoc->_versionedDoc[revidBuffer(revID)];
            return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadSelectedRevBody(outError));
        } else {
            idoc->selectRevision(NULL);
            return true;
        }
    } catchError(outError);
    return false;
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    return internal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    return internal(doc)->loadSelectedRevBody(outError);
}


bool c4doc_hasRevisionBody(C4Document* doc) {
    try {
        auto idoc = internal(doc);
        if (!idoc->revisionsLoaded()) {
            Warn("c4doc_hasRevisionBody called on doc loaded without kC4IncludeBodies");
        }
        return idoc->_selectedRev && idoc->_selectedRev->isBodyAvailable();
    } catchError(NULL);
    return false;
}


bool c4doc_selectParentRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->parent());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->next());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    auto rev = idoc->_selectedRev;
    if (rev) {
        do {
            rev = rev->next();
        } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
    }
    return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadSelectedRevBody(outError));
}


unsigned c4rev_getGeneration(C4Slice revID) {
    try {
        return revidBuffer(revID).generation();
    }catchError(NULL)
    return 0;
}


#pragma mark - INSERTING REVISIONS


int32_t c4doc_insertRevision(C4Document *doc,
                             C4Slice revID,
                             C4Slice body,
                             bool deleted,
                             bool hasAttachments,
                             bool allowConflict,
                             C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        revidBuffer encodedRevID(revID);
        int httpStatus;
        auto newRev = idoc->_versionedDoc.insert(encodedRevID,
                                                 body,
                                                 deleted,
                                                 hasAttachments,
                                                 idoc->_selectedRev,
                                                 allowConflict,
                                                 httpStatus);
        if (newRev) {
            // Success:
            idoc->updateMeta();
            newRev = idoc->_versionedDoc.get(encodedRevID);
            idoc->selectRevision(newRev);
            return 1;
        } else if (httpStatus == 200) {
            // Revision already exists, so nothing was added. Not an error.
            c4doc_selectRevision(doc, revID, true, outError);
            return 0;
        }
        recordHTTPError(httpStatus, outError);
    } catchError(outError)
    return -1;
}


int32_t c4doc_insertRevisionWithHistory(C4Document *doc,
                                        C4Slice body,
                                        bool deleted,
                                        bool hasAttachments,
                                        C4Slice history[],
                                        size_t historyCount,
                                        C4Error *outError)
{
    if (historyCount < 1)
        return 0;
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    int32_t commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers(historyCount);
        for (size_t i = 0; i < historyCount; i++)
            revIDBuffers[i].parse(history[i]);
        commonAncestor = idoc->_versionedDoc.insertHistory(revIDBuffers,
                                                           body,
                                                           deleted,
                                                           hasAttachments);
        if (commonAncestor >= 0) {
            idoc->updateMeta();
            idoc->selectRevision(idoc->_versionedDoc[revidBuffer(history[0])]);
        } else {
            recordHTTPError(400, outError); // must be invalid revision IDs
        }
    } catchError(outError)
    return commonAncestor;
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        int32_t total = idoc->_versionedDoc.purge(revidBuffer(revID));
        if (total > 0) {
            idoc->updateMeta();
            if (idoc->_selectedRevIDBuf == revID)
                idoc->selectRevision(idoc->_versionedDoc.currentRevision());
        }
        return total;
    } catchError(outError)
    return -1;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->_versionedDoc.docType().copy();
    return {result.buf, result.size};
}

bool c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError) {
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return false;
    idoc->_versionedDoc.setDocType(docType);
    return true;
}


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        idoc->save(maxRevTreeDepth);
        return true;
    } catchError(outError)
    return false;
}
