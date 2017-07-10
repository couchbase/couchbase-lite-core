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
#include "Record.hh"
#include "KeyStore.hh"
#include "Error.hh"
#include "varint.hh"
#include <ostream>

namespace litecore {
    using namespace fleece;

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

    VersionedDocument::VersionedDocument(const VersionedDocument &other)
    :RevTree(other)
    ,_db(other._db)
    ,_rec(other._rec)
    { }

    void VersionedDocument::read() {
        _db.read(_rec);
        decode();
    }

    void VersionedDocument::decode() {
        _unknown = false;
        if (_rec.body().buf) {
            RevTree::decode(_rec.body(), _rec.sequence());
            // The kSynced flag is set when the document's current revision is pushed to a server.
            // This is done instead of updating the doc body, for reasons of speed. So when loading
            // the document, detect that flag and belatedly update the current revision's flags.
            // Since the revision is now likely stored on the server, it may be the base of a merge
            // in the future, so preserve its body:
            if (_rec.flags() & DocumentFlags::kSynced)
                markCurrentRevision(Rev::kKeepBody);
        } else if (_rec.bodySize() > 0) {
            _unknown = true;        // i.e. rec was read as meta-only
        }
    }

    void VersionedDocument::updateMeta() {
        _rec.setFlags(DocumentFlags::kNone);
        const Rev* curRevision = currentRevision();
        if (curRevision) {
            _rec.setVersion(curRevision->revID);

            // Compute flags:
            if (curRevision->isDeleted())
                _rec.setFlag(DocumentFlags::kDeleted);
            if (hasConflict())
                _rec.setFlag(DocumentFlags::kConflicted);
            for (auto rev : allRevisions()) {
                if (rev->hasAttachments()) {
                    _rec.setFlag(DocumentFlags::kHasAttachments);
                    break;
                }
            }
        } else {
            _rec.setFlag(DocumentFlags::kDeleted);
            _rec.setVersion(nullslice);
        }
    }

    bool VersionedDocument::save(Transaction& transaction) {
        if (!_changed)
            return true;
        updateMeta();
        sequence_t seq = _rec.sequence();
        if (currentRevision()) {
            removeNonLeafBodies();
            // Don't call _rec.setBody() because it'll invalidate all the pointers from Revisions
            // into the existing body buffer.
            seq = _db.set(_rec.key(), _rec.version(), encode(), _rec.flags(),
                          transaction, &seq);
            if (!seq)
                return false;               // Conflict
            _rec.updateSequence(seq);
            _rec.setExists();
            saved(seq);
        } else {
            if (seq && !_db.del(_rec.key(), transaction, &seq))
                return false;
        }
        _changed = false;
        return true;
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
