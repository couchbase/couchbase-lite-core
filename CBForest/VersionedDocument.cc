//
//  VersionedDocument.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "VersionedDocument.hh"
#include <assert.h>
#include <ostream>

namespace forestdb {

    VersionedDocument::VersionedDocument(KeyStore db, slice docID)
    :_db(db), _doc(docID)
    {
        read();
    }

    VersionedDocument::VersionedDocument(KeyStore db, const Document& doc)
    :_db(db), _doc(doc)
    {
        decode();
    }

    VersionedDocument::VersionedDocument(KeyStore db, Document&& doc)
    :_db(db), _doc(std::move(doc))
    {
        decode();
    }

    void VersionedDocument::read() {
        _db.read(_doc);
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
        if (RevTree::isBodyOfRevisionAvailable(rev, atOffset))
            return true;
        if (atOffset == 0)
            return false;
        VersionedDocument oldVersDoc(_db, _db.getByOffset(atOffset, rev->sequence));
        if (oldVersDoc.sequence() != rev->sequence)
            return false;
        const Revision* oldRev = oldVersDoc.get(rev->revID);
        return (oldRev && RevTree::isBodyOfRevisionAvailable(oldRev, atOffset));
    }

    alloc_slice VersionedDocument::readBodyOfRevision(const Revision* rev, uint64_t atOffset) const {
        if (RevTree::isBodyOfRevisionAvailable(rev, atOffset))
            return RevTree::readBodyOfRevision(rev, atOffset);
        if (atOffset == 0)
            return alloc_slice();
        VersionedDocument oldVersDoc(_db, _db.getByOffset(atOffset, rev->sequence));
        if (oldVersDoc.sequence() != rev->sequence)
            return alloc_slice();
        const Revision* oldRev = oldVersDoc.get(rev->revID);
        if (!oldRev)
            return alloc_slice();
        return alloc_slice(oldRev->inlineBody());
    }

    void VersionedDocument::save(Transaction& transaction) {
        if (!_changed)
            return;
        updateMeta();
        // Don't call _doc.setBody() because it'll invalidate all the pointers from Revisions into
        // the existing body buffer.
        _doc.updateSequence( transaction(_db).set(_doc.key(), _doc.meta(), encode()) );
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
