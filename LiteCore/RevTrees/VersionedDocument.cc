//
//  VersionedDocument.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
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

namespace litecore {
    using namespace fleece;

    /* VersionedDocument metadata has the following structure:
        1 byte  flags
        1 byte  revid length
        bytes   revid
        varint  type length
        bytes   type
    */

    VersionedDocument::VersionedDocument(KeyStore& db, slice docID)
    :_db(db), _rec(docID)
    {
        read();
    }

    VersionedDocument::VersionedDocument(KeyStore& db, const Record& rec)
    :_db(db), _rec(std::move(rec))
    {
        decode();
    }

    void VersionedDocument::read() {
        _db.read(_rec);
        decode();
    }

    void VersionedDocument::decode() {
        _unknown = false;
        if (_rec.body().buf)
            RevTree::decode(_rec.body(), _rec.sequence());
        else if (_rec.bodySize() > 0)
            _unknown = true;        // i.e. rec was read as meta-only

        if (_rec.exists()) {
            slice docType;
            if (!readMeta(_rec, _flags, _revID, docType))
                error::_throw(error::CorruptRevisionData);
            _recType = docType; // allocate buf for it
        } else {
            _flags = 0;
        }
    }

    bool VersionedDocument::readMeta(const Record& rec,
                                     Flags& flags, revid& revID, slice& docType)
    {
        slice meta = rec.meta();
        if (meta.size < 2)
            return false;
        flags = meta.read(1)[0];
        uint8_t length = meta.read(1)[0];
        revID = revid(meta.read(length));
        if (!revID.buf)
            error::_throw(error::CorruptRevisionData);
        if (meta.size > 0) {
            uint64_t docTypeLength;
            if (!ReadUVarInt(&meta, &docTypeLength))
                error::_throw(error::CorruptRevisionData);
            docType = meta.read((size_t)docTypeLength);
        } else {
            docType = nullslice;
        }
        return true;
    }

    void VersionedDocument::updateMeta() {
        slice revID;
        Flags flags = 0;

        const Rev* curRevision = currentRevision();
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

        // Write to _rec.meta:
        slice meta = _rec.resizeMeta(2 + revID.size + SizeOfVarInt(_recType.size) + _recType.size);
        meta.writeFrom(slice(&flags,1));
        uint8_t revIDSize = (uint8_t)revID.size;
        meta.writeFrom(slice(&revIDSize, 1));
        _revID = revid(meta.buf, revID.size);
        meta.writeFrom(revID);
        WriteUVarInt(&meta, _recType.size);
        meta.writeFrom(_recType);
        Assert(meta.size == 0);
    }

    void VersionedDocument::save(Transaction& transaction) {
        if (!_changed)
            return;
        updateMeta();
        if (currentRevision()) {
            removeNonLeafBodies();
            // Don't call _rec.setBody() because it'll invalidate all the pointers from Revisions
            // into the existing body buffer.
            auto result = _db.set(_rec.key(), _rec.meta(), encode(), transaction);
            _rec.updateSequence(result.seq);
        } else {
            _db.del(_rec.key(), transaction);
        }
        saved();
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
