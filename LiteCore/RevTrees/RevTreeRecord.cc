//
// RevTreeRecord.cc
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

#include "RevTreeRecord.hh"
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
        read(content);
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
    {
        updateScope();
    }

    RevTreeRecord::~RevTreeRecord() {
        _fleeceScopes.clear(); // do this before the memory is freed (by _rec)
    }

    void RevTreeRecord::read(ContentOption content) {
        _store.read(_rec, content);
        decode();
    }

    void RevTreeRecord::decode() {
        _unknown = false;
        updateScope();

        if (_rec.exists()) {
            _contentLoaded = _rec.contentLoaded();
            switch (_contentLoaded) {
                case kEntireBody:
                    RevTree::decode(_rec.body(), _rec.extra(),  _rec.sequence());
                    if (auto cur = currentRevision(); cur && (_rec.flags() & DocumentFlags::kSynced)) {
                        // The kSynced flag is set when the document's current revision is pushed to a server.
                        // This is done instead of updating the doc body, for reasons of speed. So when loading
                        // the document, detect that flag and belatedly update the current revision's flags.
                        // Since the revision is now likely stored on the server, it may be the base of a merge
                        // in the future, so preserve its body:
                        setLatestRevisionOnRemote(kDefaultRemoteID, cur);
                        keepBody(cur);
                        _changed = false;
                    }

                    // If there is no `extra`, this record is being upgraded from v2.x and must be saved:
                    if (!_rec.extra())
                        _changed = true;
                    break;
                case kCurrentRevOnly: {
#if 1
                    _unknown = true;
#else
                    // Only the current revision body is loaded, not the tree. Create a fake Rev:
                    Rev::Flags flags = {};
                    int status;
                    insert(revid(_rec.version()),
                           _rec.body(),
                           flags,
                           revid(),
                           false, false, status);
                    Assert(status == 200);
#endif
                    break;
                }
                case kMetaOnly:
                    _unknown = true;        // i.e. rec was read as meta-only
                    break;
            }
        } else {
            _contentLoaded = kEntireBody;
        }
    }

    slice RevTreeRecord::currentRevBody() {
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

    RevTreeRecord::SaveResult RevTreeRecord::save(Transaction& transaction) {
        Assert(revsAvailable());
        if (!_changed)
            return kNoNewSequence;
        updateMeta();
        sequence_t seq = _rec.sequence();
        bool createSequence;
        if (auto cur = currentRevision(); cur) {
            createSequence = (seq == 0 || hasNewRevisions());
            removeNonLeafBodies();
            slice newBody;
            alloc_slice newExtra;
            std::tie(newBody, newExtra) = encode();

            RecordLite newRec;
            newRec.key = _rec.key();
            newRec.version = _rec.version();
            newRec.flags = _rec.flags();
            newRec.sequence = seq;
            newRec.updateSequence = createSequence;
            newRec.body = newBody;
            newRec.extra = newExtra;

            seq = _store.set(newRec, transaction);
            if (!seq)
                return kConflict;               // Conflict

            _rec.updateSequence(seq);
            _rec.setExists();
            // (Don't update _rec body or extra, because it'd invalidate all the inner pointers from
            // Rev objects into the existing body/extra buffer.)
            LogVerbose(DBLog, "Saved doc '%.*s' #%s; body=%zu, extra=%zu",
                  SPLAT(newRec.key), revid(newRec.version).str().c_str(),
                  newRec.body.size, newRec.extra.size);
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
    void RevTreeRecord::dump(std::ostream& out) {
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
