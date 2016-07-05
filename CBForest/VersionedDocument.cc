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
#include "Error.hh"
#include "varint.hh"
#include <ostream>

namespace cbforest {

    /* VersionedDocument metadata has the following structure:
        1 byte  flags
        1 byte  revid length
        bytes   revid
        varint  type length
        bytes   type
    */

    VersionedDocument::VersionedDocument(KeyStore& db, slice docID)
    :_db(db), _doc(docID)
    {
        read();
    }

    VersionedDocument::VersionedDocument(KeyStore& db, Document&& doc)
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

        if (_doc.exists()) {
            slice docType;
            if (!readMeta(_doc, _flags, _revID, docType))
                throw error(error::CorruptRevisionData);
            _docType = docType; // allocate buf for it
        } else {
            _flags = 0;
        }
    }

    bool VersionedDocument::readMeta(const Document& doc,
                                     Flags& flags, revid& revID, slice& docType)
    {
        slice meta = doc.meta();
        if (meta.size < 2)
            return false;
        flags = meta.read(1)[0];
        uint8_t length = meta.read(1)[0];
        revID = revid(meta.read(length));
        if (!revID.buf)
            throw error(error::CorruptRevisionData);
        if (meta.size > 0) {
            uint64_t docTypeLength;
            if (!ReadUVarInt(&meta, &docTypeLength))
                throw error(error::CorruptRevisionData);
            docType = meta.read((size_t)docTypeLength);
        } else {
            docType = slice::null;
        }
        return true;
    }

    void VersionedDocument::updateMeta() {
        slice revID;
        Flags flags = 0;

        const Revision* curRevision = currentRevision();
        if (curRevision) {
            revID = curRevision->revID;

            // Compute flags:
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
        } else {
            flags = kDeleted;
        }

        // update _flags instance variable
        _flags = flags;

        // Write to _doc.meta:
        slice meta = _doc.resizeMeta(2 + revID.size + SizeOfVarInt(_docType.size) + _docType.size);
        meta.writeFrom(slice(&flags,1));
        uint8_t revIDSize = (uint8_t)revID.size;
        meta.writeFrom(slice(&revIDSize, 1));
        _revID = revid(meta.buf, revID.size);
        meta.writeFrom(revID);
        WriteUVarInt(&meta, _docType.size);
        meta.writeFrom(_docType);
        CBFAssert(meta.size == 0);
    }

    bool VersionedDocument::isBodyOfRevisionAvailable(const Revision* rev, uint64_t atOffset) const {
        if (RevTree::isBodyOfRevisionAvailable(rev, atOffset))
            return true;
        if (atOffset == 0 || atOffset >= _doc.offset())
            return false;
        VersionedDocument oldVersDoc(_db, _db.getByOffsetNoErrors(atOffset, rev->sequence));
        if (!oldVersDoc.exists() || oldVersDoc.sequence() != rev->sequence)
            return false;
        const Revision* oldRev = oldVersDoc.get(rev->revID);
        return (oldRev && RevTree::isBodyOfRevisionAvailable(oldRev, atOffset));
    }

    alloc_slice VersionedDocument::readBodyOfRevision(const Revision* rev, uint64_t atOffset) const {
        if (RevTree::isBodyOfRevisionAvailable(rev, atOffset))
            return RevTree::readBodyOfRevision(rev, atOffset);
        if (atOffset == 0 || atOffset >= _doc.offset())
            return alloc_slice();
        VersionedDocument oldVersDoc(_db, _db.getByOffsetNoErrors(atOffset, rev->sequence));
        if (!oldVersDoc.exists() || oldVersDoc.sequence() != rev->sequence)
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
        if (currentRevision()) {
            // Don't call _doc.setBody() because it'll invalidate all the pointers from Revisions into
            // the existing body buffer.
            _doc.updateSequence( transaction(_db).set(_doc.key(), _doc.meta(), encode()) );
        } else {
            transaction(_db).del(_doc.key());
        }
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
