//
// VersionedDocument.cc
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#include "VersionedDocument.hh"
#include "Record.hh"
#include "KeyStore.hh"
#include "DataFile.hh"
#include "Error.hh"
#include "Doc.hh"
#include "varint.hh"
#include "MutableArray.hh"
#include "MutableDict.hh"
#include <ostream>

namespace litecore {
    using namespace fleece;
    using namespace fleece::impl;

    VersionedDocument::VersionedDocument(KeyStore& store, slice docID)
    :_store(store), _rec(docID)
    {
        read();
    }

    VersionedDocument::VersionedDocument(KeyStore& store, const Record& rec)
    :_store(store), _rec(std::move(rec))
    {
        decode();
    }

    VersionedDocument::VersionedDocument(const VersionedDocument &other)
    :RevTree(other)
    ,_store(other._store)
    ,_rec(other._rec)
    {
        updateScope();
    }

    VersionedDocument::~VersionedDocument() {
        _fleeceScopes.clear(); // do this before the memory is freed (by _rec)
    }

    void VersionedDocument::read() {
        _store.read(_rec);
        decode();
    }

    void VersionedDocument::decode() {
        _unknown = false;
        updateScope();
        if (_rec.body().buf) {
            RevTree::decode(_rec.body(), _rec.sequence());
            // The kSynced flag is set when the document's current revision is pushed to a server.
            // This is done instead of updating the doc body, for reasons of speed. So when loading
            // the document, detect that flag and belatedly update the current revision's flags.
            // Since the revision is now likely stored on the server, it may be the base of a merge
            // in the future, so preserve its body:
            if (_rec.flags() & DocumentFlags::kSynced) {
                setLatestRevisionOnRemote(kDefaultRemoteID, currentRevision());
                keepBody(currentRevision());
                _changed = false;
            }
        } else if (_rec.bodySize() > 0) {
            _unknown = true;        // i.e. rec was read as meta-only
        }
    }

    void VersionedDocument::updateScope() {
        Assert(_fleeceScopes.empty());
        addScope(_rec.body());
    }

    alloc_slice VersionedDocument::addScope(const alloc_slice &body) {
        // A Scope associates the SharedKeys with the Fleece data in the body, so Fleece Dict
        // accessors can decode the int keys.
        if (body)
            _fleeceScopes.push_back(new VersFleeceDoc(body, _store.dataFile().documentKeys(),
                                                      this));
        return body;
    }

    VersionedDocument* VersionedDocument::containing(const Value *value) {
        if (value->isMutable()) {
            // Scope doesn't know about mutable Values (they're in the heap), but the mutable
            // Value may be a mutable copy of a Value with scope...
            if (value->asDict())
                value = value->asDict()->asMutable()->source();
            else
                value = value->asArray()->asMutable()->source();
            if (!value)
                return nullptr;
        }
        
        const Scope *scope = fleece::impl::Scope::containing(value);
        if (!scope)
            return nullptr;
        auto versScope = dynamic_cast<const VersFleeceDoc*>(scope);
        if (!versScope)
            return nullptr;
        return versScope->document;
    }

    alloc_slice VersionedDocument::copyBody(slice body) {
        return addScope(RevTree::copyBody(body));
    }

    alloc_slice VersionedDocument::copyBody(const alloc_slice &body) {
        return addScope(RevTree::copyBody(body));
    }

    Retained<fleece::impl::Doc> VersionedDocument::fleeceDocFor(slice s) const {
        if (!s)
            return nullptr;
        for (auto &doc : _fleeceScopes) {
            if (doc->data().containsAddressRange(s))
                return new Doc(doc, s, Doc::kTrusted);
        }
        error::_throw(error::AssertionFailed,
                      "VersionedDocument has no fleece::Doc containing slice");
    }


    bool VersionedDocument::updateMeta() {
        auto oldFlags = _rec.flags();
        alloc_slice oldRevID = _rec.version();

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

        return _rec.flags() != oldFlags || _rec.version() != oldRevID;
    }

    VersionedDocument::SaveResult VersionedDocument::save(Transaction& transaction) {
        if (_usuallyFalse(ensureConflictStateConsistent())) {
            Warn("Document '%.*s' recovered from inconsistent state", SPLAT(docID()));
        }
        if (!_changed)
            return kNoNewSequence;
        updateMeta();
        sequence_t seq = _rec.sequence();
        bool createSequence;
        if (currentRevision()) {
            removeNonLeafBodies();
            auto newBody = encode();
            createSequence = seq == 0 || hasNewRevisions();
            // (Don't call _rec.setBody(), because it'd invalidate all the inner pointers from
            // Revs into the existing body buffer.)
            seq = _store.set(_rec.key(), _rec.version(), newBody, _rec.flags(),
                          transaction, &seq, createSequence);
            if (!seq)
                return kConflict;               // Conflict
            _rec.updateSequence(seq);
            _rec.setExists();
            if (createSequence)
                saved(seq);
        } else {
            createSequence = false;
            if (seq && !_store.del(_rec.key(), transaction, seq))
                return kConflict;
        }
        _changed = false;
        return createSequence ? kNewSequence : kNoNewSequence;
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
