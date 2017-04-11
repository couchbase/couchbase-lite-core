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
#include "DocumentMeta.hh"
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
            _meta.decode(_rec.meta());
        } else {
            _meta.flags = DocumentFlags::kNone;
        }
    }

    void VersionedDocument::updateMeta() {
        _meta.flags = kNone;
        const Rev* curRevision = currentRevision();
        if (curRevision) {
            _meta.version = curRevision->revID;

            // Compute flags:
            if (curRevision->isDeleted())
                _meta.setFlag(DocumentFlags::kDeleted);
            if (hasConflict())
                _meta.setFlag(DocumentFlags::kConflicted);
            for (auto rev=allRevisions().begin(); rev != allRevisions().end(); ++rev) {
                if (rev->hasAttachments()) {
                    _meta.setFlag(DocumentFlags::kHasAttachments);
                    break;
                }
            }
        } else {
            _meta.setFlag(kDeleted);
            _meta.version = nullslice;
        }

        // Write to _rec.meta:
        _rec.setMeta(_meta.encodeAndUpdate());
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
            saved(result.seq);
        } else {
            _db.del(_rec.key(), transaction);
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
