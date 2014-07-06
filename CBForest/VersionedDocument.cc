//
//  VersionedDocument.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "VersionedDocument.hh"
#include <assert.h>
#include <ostream>

namespace forestdb {

    VersionedDocument::VersionedDocument(Database* db, slice docID)
    :_db(db), _doc(docID)
    {
        read();
    }

    VersionedDocument::VersionedDocument(Database* db, const Document& doc)
    :_db(db), _doc(doc)
    {
        decode();
    }

    VersionedDocument::VersionedDocument(Database* db, Document&& doc)
    :_db(db), _doc(std::move(doc))
    {
        decode();
    }

    void VersionedDocument::read() {
        _db->read(_doc);
        decode();
    }

    void VersionedDocument::decode() {
        _unknown = false;
        if (_doc.body().buf)
            RevTree::decode(_doc.body(), _doc.sequence(), _doc.offset());
        else if (_doc.body().size > 0)
            _unknown = true;        // i.e. doc was read as meta-only
    }

    revid VersionedDocument::revID() const {
        slice result = _doc.meta();
        if (result.size <= 1)
            return revid();
        result.moveStart(1);
        return revid(result);
    }

    VersionedDocument::Flags VersionedDocument::flags() const {
        return flagsOfDocument(_doc);
    }

    VersionedDocument::Flags VersionedDocument::flagsOfDocument(const Document& doc) {
        slice meta = doc.meta();
        if (meta.size < 1)
            return 0;
        return meta[0];
    }

    void VersionedDocument::updateMeta() {
        const Revision* curRevision = currentRevision();
        slice revID = curRevision->revID;
        Flags flags = 0;
        if (curRevision->isDeleted())
            flags |= kDeleted;
        if (hasConflict())
            flags |= kConflicted;

        for (auto rev=allRevisions().begin(); rev != allRevisions().end(); ++rev) {
            if (rev->hasAttachments()) {
                flags |= kHasAttachments;
                break;
            }
        }

        slice meta = _doc.resizeMeta(1+revID.size);
        (uint8_t&)meta[0] = flags;
        memcpy((void*)&meta[1], revID.buf, revID.size);
    }

    bool VersionedDocument::isBodyOfRevisionAvailable(const Revision* rev, uint64_t atOffset) const {
        if (rev->body.buf)
            return true;
        if (atOffset == 0)
            return false;
        VersionedDocument oldVersDoc(_db, _db->getByOffset(atOffset, rev->sequence));
        if (oldVersDoc.sequence() != rev->sequence)
            return false;
        rev = oldVersDoc.get(rev->revID);
        return (rev && rev->body.buf);
    }

    alloc_slice VersionedDocument::readBodyOfRevision(const Revision* rev, uint64_t atOffset) const {
        if (rev->body.buf)
            return alloc_slice(rev->body);
        if (atOffset == 0)
            return alloc_slice();
        VersionedDocument oldVersDoc(_db, _db->getByOffset(atOffset, rev->sequence));
        if (oldVersDoc.sequence() != rev->sequence)
            return alloc_slice();
        rev = oldVersDoc.get(rev->revID);
        if (!rev)
            return alloc_slice();
        return alloc_slice(rev->body);
    }

    void VersionedDocument::save(Transaction& transaction) {
        if (!_changed)
            return;
        updateMeta();
        // Don't call _doc.setBody() because it'll invalidate all the pointers from Revisions into
        // the existing body buffer.
        _doc.updateSequence( transaction.set(_doc.key(), _doc.meta(), encode()) );
        _changed = false;
    }

#if DEBUG
    void VersionedDocument::dump(std::ostream& out) {
        out << "\"" << (std::string)docID() << "\" / " << (std::string)revID();
        out << " (seq " << sequence() << ") ";
        if (isDeleted())
            out << " del";
        if (isConflicted())
            out << " conflicted";
        if (hasAttachments())
            out << " attachments";
        out << "\n";
        RevTree::dump(out);
    }
#endif

}
