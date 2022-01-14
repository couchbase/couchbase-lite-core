//
// RevTreeRecord.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RevTreeRecord.hh"
#include "RawRevTree.hh"
#include "Record.hh"
#include "KeyStore.hh"
#include "DataFile.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Doc.hh"
#include "varint.hh"
#include "MutableArray.hh"
#include "MutableDict.hh"
#include <ostream>

namespace litecore {
    using namespace fleece;
    using namespace fleece::impl;

    RevTreeRecord::RevTreeRecord(KeyStore& store, slice docID, ContentOption content)
    :_store(store), _rec(docID)
    {
        (void)read(content);
    }

    RevTreeRecord::RevTreeRecord(KeyStore& store, const Record& rec)
    :_store(store), _rec(rec)
    {
        decode();
    }

    RevTreeRecord::RevTreeRecord(const RevTreeRecord &other)
    :RevTree(other)
    ,_store(other._store)
    ,_rec(other._rec)
    ,_contentLoaded(other._contentLoaded)
    {
        updateScope();
    }

    RevTreeRecord::~RevTreeRecord() {
        _fleeceScopes.clear(); // do this before the memory is freed (by _rec)
    }

    bool RevTreeRecord::read(ContentOption content) {
        if (_rec.sequence() > 0_seq) {
            if (!_store.read(_rec, ReadBy::Sequence, content))
                return false;
        } else {
            _store.read(_rec, ReadBy::Key, content);
        }
        decode();
        return true;
    }

    void RevTreeRecord::decode() {
        _unknown = false;
        updateScope();

        if (_rec.exists()) {
            _contentLoaded = _rec.contentLoaded();
            if (_contentLoaded == kCurrentRevOnly && RawRevision::isRevTree(_rec.body())) {
                // Only asked for the current rev, but since doc is in the v2 format we got the
                // entire rev-tree in the body:
                _contentLoaded = kEntireBody;
            }

            switch (_contentLoaded) {
                case kEntireBody:
                    RevTree::decode(_rec.body(), _rec.extra(),  _rec.sequence());
                    if (auto cur = currentRevision(); cur && (_rec.flags() & DocumentFlags::kSynced)) {
                        // The kSynced flag is set when the document's current revision is pushed
                        // to a server. This is done instead of updating the doc body, for reasons
                        // of speed. So when loading the document, detect that flag and belatedly
                        // update the current revision's flags. Since the revision is now likely
                        // stored on the server, it may be the base of a merge in the future,
                        // so preserve its body:
                        setLatestRevisionOnRemote(kDefaultRemoteID, cur);
                        _rec.clearFlag(DocumentFlags::kSynced);
                        keepBody(cur);
                        _changed = false;
                    }

                    // If there is no `extra`, this record is being upgraded from v2.x and must be saved:
                    if (!_rec.extra())
                        _changed = true;
                    break;
                case kCurrentRevOnly:
                case kMetaOnly:
                    _unknown = true;
                    break;
            }
        } else {
            _contentLoaded = kEntireBody;
        }
    }

    slice RevTreeRecord::currentRevBody() const {
        if (revsAvailable())
            return currentRevision()->body();
        else {
            Assert(currentRevAvailable());
            return _rec.body();
        }
    }

    void RevTreeRecord::updateScope() {
        _fleeceScopes.clear();
        addScope(_rec.body());
        if (_rec.extra())
            addScope(_rec.extra());
    }

    alloc_slice RevTreeRecord::addScope(const alloc_slice &body) {
        // A Scope associates the SharedKeys with the Fleece data in the body, so Fleece Dict
        // accessors can decode the int keys.
        if (body)
            _fleeceScopes.push_back(new VersFleeceDoc(body, _store.dataFile().documentKeys(),
                                                      this));
        return body;
    }

    RevTreeRecord* RevTreeRecord::containing(const Value *value) {
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

    alloc_slice RevTreeRecord::copyBody(slice body) {
        return addScope(RevTree::copyBody(body));
    }

    alloc_slice RevTreeRecord::copyBody(const alloc_slice &body) {
        return addScope(RevTree::copyBody(body));
    }

    Retained<fleece::impl::Doc> RevTreeRecord::fleeceDocFor(slice s) const {
        if (!s)
            return nullptr;
        for (auto &doc : _fleeceScopes) {
            if (doc->data().containsAddressRange(s))
                return new Doc(doc, s, Doc::kTrusted);
        }
        error::_throw(error::AssertionFailed,
                      "RevTreeRecord has no fleece::Doc containing slice");
    }


    bool RevTreeRecord::updateMeta() {
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

    RevTreeRecord::SaveResult RevTreeRecord::save(ExclusiveTransaction& transaction) {
        Assert(revsAvailable());
        if (!_changed)
            return kNoNewSequence;
        updateMeta();
        sequence_t sequence = _rec.sequence();
        bool createSequence;
        if (auto cur = currentRevision(); cur) {
            createSequence = (sequence == 0_seq || hasNewRevisions());
            removeNonLeafBodies();
            slice newBody;
            alloc_slice newExtra;
            std::tie(newBody, newExtra) = encode();

            RecordUpdate newRec(_rec);
            newRec.body = newBody;
            newRec.extra = newExtra;

            sequence = _store.set(newRec, createSequence, transaction);
            if (sequence == 0_seq)
                return kConflict;               // Conflict

            if (createSequence)
                _rec.updateSequence(sequence);
            else
                _rec.updateSubsequence();
            _rec.setExists();
            
            // (Don't update _rec body or extra, because it'd invalidate all the inner pointers from
            // Rev objects into the existing body/extra buffer.)
            LogVerbose(DBLog, "Saved doc '%.*s' #%s; body=%zu, extra=%zu",
                  SPLAT(newRec.key), revid(newRec.version).str().c_str(),
                  newRec.body.size, newRec.extra.size);
            if (createSequence)
                saved(sequence);
        } else {
            createSequence = false;
            if (sequence != 0_seq && !_store.del(_rec.key(), transaction, sequence))
                return kConflict;
        }
        _changed = false;
        return createSequence ? kNewSequence : kNoNewSequence;
    }

#if DEBUG
    void RevTreeRecord::dump(std::ostream& out) {
        out << "\"" << (std::string)docID() << "\" / " << (std::string)revID();
        out << " (seq " << uint64_t(sequence()) << ") ";
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
