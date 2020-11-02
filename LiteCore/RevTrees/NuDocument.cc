//
// NuDocument.cc
//
// Copyright (c) 2020 Couchbase, Inc All rights reserved.
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

#include "NuDocument.hh"
#include "Record.hh"
#include "KeyStore.hh"
#include "DataFile.hh"
#include "DeDuplicateEncoder.hh"
#include "Error.hh"
#include "Defer.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include <ostream>
#include <sstream>

namespace litecore {
    using namespace std;
    using namespace fleece;

    /*
     RECORD BODY FORMAT:

     A record body (i.e. the `body` column of the `kv_default` table) is a Fleece-encoded array.
     Each item of the array describes a revision.
     The first item is the local current revision.
     Other revisions are the known versions at remote peers, indexed by RemoteID values.
     Each revision is a Dict with keys:
        - `body` (document body, itself a Dict),
        - `revID` (revision ID, binary data),
        - `flags` (DocumentFlags, int, omitted if 0)
     It's very common for two or more revisions to be the same, or at least have a lot of property
     values in common. Thus, when encoding the record body we use a DeDuplicatingEncoder to save
     space, writing repeated values only once.
     */

    // Properties in revision dicts:
    static constexpr slice kMetaBody  = "body";
    static constexpr slice kMetaRevID = "revID";
    static constexpr slice kMetaFlags = "flags";


    NuDocument::NuDocument(KeyStore& store, const Record& rec)
    :_store(store)
    ,_docID(rec.key())
    ,_sequence(rec.sequence())
    {
        if (initFleeceDoc(rec.body())) {
            if (!_revisions)
                error::_throw(error::CorruptRevisionData);
            _properties = _revisions[int(RemoteID::Local)].asDict()[kMetaBody].asDict();
            if (!_properties)
                error::_throw(error::CorruptRevisionData);
        } else {
            // "Untitled" empty state:
            (void)mutableProperties();
        }
    }


    NuDocument::NuDocument(KeyStore& store, slice docID)
    :NuDocument(store, store.get(docID))
    { }


    NuDocument::~NuDocument() = default;


#pragma mark - REVISIONS:


//    void NuDocument::readRevisions(alloc_slice &body) {
//        if (initFleeceDoc(body)) {
//            if (!_revisions)
//                error::_throw(error::CorruptRevisionData);
//            _properties = _revisions[int(RemoteID::Local)].asDict()[kMetaBody].asDict();
//            if (!_properties)
//                error::_throw(error::CorruptRevisionData);
//        } else {
//            // "Untitled" empty state:
//            (void)mutableProperties();
//        }
//    }


    optional<Revision> NuDocument::remoteRevision(RemoteID remote) const {
        if (Dict revDict = _revisions[int(remote)].asDict(); revDict) {
            // revisions have a top-level dict with the revID, flags, properties.
            Dict properties = revDict[kMetaBody].asDict();
            revid revID(revDict[kMetaRevID].asData());
            auto flags = DocumentFlags(revDict[kMetaFlags].asInt());
            if (!properties)
                properties = Dict::emptyDict();
            if (!revID && remote != RemoteID::Local)
                error::_throw(error::CorruptRevisionData);
            return Revision{properties, revID, flags};
        } else {
            return nullopt;
        }
    }


    // If _revisions is not mutable, makes a mutable copy and assigns it to _mutatedRevisions.
    void NuDocument::mutateRevisions() {
        if (!_mutatedRevisions) {
            _mutatedRevisions = _revisions ? _revisions.mutableCopy() : MutableArray::newArray();
            _revisions = _mutatedRevisions;
        }
    }

    // Returns the MutableDict for a revision.
    // If it's not mutable yet, replaces it with a mutable copy.
    MutableDict NuDocument::mutableRevisionDict(RemoteID remote) {
        mutateRevisions();
        if (_mutatedRevisions.count() <= int(remote))
            _mutatedRevisions.resize(int(remote) + 1);
        MutableDict revDict = _mutatedRevisions.getMutableDict(int(remote));
        if (!revDict)
            _mutatedRevisions[int(remote)] = revDict = MutableDict::newDict();
        return revDict;
    }


    // Updates a revision. (Local changes, e.g. setRevID and setFlags, go through this too.)
    void NuDocument::setRemoteRevision(RemoteID remote, const optional<Revision> &optRev) {
        if (auto &newRev = *optRev; optRev) {
            // Creating/updating a revision (possibly the local one):
            MutableDict revDict = mutableRevisionDict(remote);
            if (auto oldRevID = revDict[kMetaRevID].asData(); newRev.revID != oldRevID) {
                if (!newRev.revID)
                    error::_throw(error::CorruptRevisionData);
                revDict[kMetaRevID].setData(newRev.revID);
                _changed = true;
                if (remote == RemoteID::Local)
                    _revIDChanged = true;
            }
            if (newRev.properties != revDict[kMetaBody]) {
                revDict[kMetaBody] = newRev.properties;
                _changed = true;
            }
            if (int(newRev.flags) != revDict[kMetaFlags].asInt()) {
                if (newRev.flags != DocumentFlags::kNone)
                    revDict[kMetaFlags] = int(newRev.flags);
                else
                    revDict.remove(kMetaFlags);
                _changed = true;
            }
        } else if (_revisions[int(remote)]) {
            // Removing a remote revision.
            // First replace its Dict with null, then remove trailing nulls from the revision array.
            Assert(remote != RemoteID::Local);
            mutateRevisions();
            _mutatedRevisions[int(remote)] = Value::null();
            auto n = _mutatedRevisions.count();
            while (n > 0 && !_mutatedRevisions[n-1].asDict())
                --n;
            _mutatedRevisions.resize(n);
            _changed = true;
        }
    }


    Dict NuDocument::originalProperties() const {
        Dict rev = savedRevisions()[int(RemoteID::Local)].asDict();
        return rev[kMetaBody].asDict();
    }


    MutableDict NuDocument::mutableProperties() {
        MutableDict mutProperties = _properties.asMutable();
        if (!mutProperties) {
            MutableDict rev = mutableRevisionDict(RemoteID::Local);
            mutProperties = rev.getMutableDict(kMetaBody);
            if (!mutProperties)
                rev[kMetaBody] = mutProperties = MutableDict::newDict();
            _properties = mutProperties;
        }
        return mutProperties;
    }


    template <class LAMBDA>
    static void mutateCurrentRevision(NuDocument *doc, LAMBDA callback) {
        Revision rev = doc->currentRevision();
        callback(rev);
        doc->setCurrentRevision(rev);
    }

    void NuDocument::setProperties(Dict newProperties) {
        mutateCurrentRevision(this, [=](Revision &rev) {rev.properties = newProperties;});
    }

    void NuDocument::setRevID(revid newRevID) {
        mutateCurrentRevision(this, [=](Revision &rev) {rev.revID = newRevID;});
    }

    void NuDocument::setFlags(DocumentFlags newFlags) {
        mutateCurrentRevision(this, [=](Revision &rev) {rev.flags = newFlags;});
    }


    bool NuDocument::changed() const {
        return _changed || propertiesChanged();
    }


    bool NuDocument::propertiesChanged() const {
        for (DeepIterator i(_properties); i; ++i) {
            if (Value val = i.value(); val.isMutable()) {
                if (auto dict = val.asDict(); dict) {
                    if (dict.asMutable().isChanged())
                        return true;
                } else if (auto array = val.asArray(); array) {
                    if (array.asMutable().isChanged())
                        return true;
                }
            } else {
                i.skipChildren();
            }
        }
        return false;
    }


    void NuDocument::clearPropertiesChanged() {
        for (DeepIterator i(_properties); i; ++i) {
            if (Value val = i.value(); val.isMutable()) {
                if (auto dict = val.asDict(); dict)
                    FLMutableDict_SetChanged(dict.asMutable(), false);
                else if (auto array = val.asArray(); array)
                    FLMutableArray_SetChanged(array.asMutable(), false);
            } else {
                i.skipChildren();
            }
        }
    }


#pragma mark - SAVING:


    NuDocument::SaveResult NuDocument::save(Transaction& transaction) {
        auto [_, revID, flags] = currentRevision();
        bool newRevision = !revID || propertiesChanged();
        if (!newRevision && !_changed)
            return kNoSave;

        // If the revID hasn't been changed but the local properties have, generate a new revID:
        revidBuffer generatedRev;
        if (newRevision && !_revIDChanged) {
            generatedRev = generateRevID(_properties, revID, flags);
            setRevID(generatedRev);
            revID = generatedRev;
            Log("Generated revID '%s'", generatedRev.str().c_str());
        }

        alloc_slice body = _encoder ? encodeBody(_encoder) : encodeBody(Encoder(_sharedKeys));

        auto seq = _sequence;
        bool newSequence = (seq == 0 || _revIDChanged);
        seq = _store.set(_docID, revID, body, flags, transaction, seq, newSequence);
        if (seq == 0)
            return kConflict;

        _sequence = seq;
        _changed = _revIDChanged = false;

        // Update Fleece Doc to newly saved data:
        MutableDict mutableProperties = _properties.asMutable();
        initFleeceDoc(body);
        if (mutableProperties) {
            // The client might still have references to mutable objects under _properties,
            // so keep that mutable Dict as the current _properties:
            auto rev = mutableRevisionDict(RemoteID::Local);
            rev[kMetaBody] = mutableProperties;
            _properties = mutableProperties;
            clearPropertiesChanged();
        } else {
            _properties = _revisions[int(RemoteID::Local)].asDict()[kMetaBody].asDict();
        }

        return newSequence ? kNewSequence : kNoNewSequence;
    }


    alloc_slice NuDocument::encodeBody(FLEncoder flEnc) {
        SharedEncoder enc(flEnc);
        unsigned nRevs = _revisions.count();
        if (nRevs == 1) {
            enc.writeValue(_revisions);
        } else {
            // If there are multiple revisions, de-duplicate as much as possible, including entire
            // revision dicts, or top-level property values in each revision.
            // Revision dicts will not be pointer-equal if revisions have been added, so we have
            // to compare them by revid. (This is O(n^2), but the number of revs is small.)
            enc.beginArray();
            DeDuplicateEncoder ddenc(enc);
            for (unsigned i = 0; i < nRevs; i++) {
                Value rev = _revisions[i];
                slice revid = rev.asDict()[kMetaRevID].asData();
                DebugAssert(revid);
                for (unsigned j = 0; j < i; j++) {
                    auto revj = _revisions[j];
                    if (revj == rev || revj.asDict()[kMetaRevID].asData() == revid) {
                        DebugAssert(revj.isEqual(rev), "RevIDs match but revisions don't");
                        rev = revj;
                        break;
                    }
                }
                // De-duplicate the revision dict itself, and the properties dict in it (depth 2)
                ddenc.writeValue(rev, 2);
            }
            enc.endArray();
        }
        return enc.finish();
    }


    revidBuffer NuDocument::generateRevID(Dict body, revid parentRevID, DocumentFlags flags) {
        // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and JSON:
        alloc_slice json = FLValue_ToJSONX(body, false, true);
        parentRevID.setSize(min(parentRevID.size, size_t(255)));
        uint8_t revLen = (uint8_t)parentRevID.size;
        uint8_t delByte = (flags & DocumentFlags::kDeleted) != 0;
        SHA1 digest = (SHA1Builder() << revLen << parentRevID << delByte << json).finish();
        unsigned generation = parentRevID ? parentRevID.generation() + 1 : 1;
        return revidBuffer(generation, slice(digest), kDigestType);
    }


#pragma mark - TESTING:


    void NuDocument::dump(ostream& out) const {
        out << "\"" << (string)docID() << "\" #" << sequence() << " ";
        int nRevs = _revisions.count();
        for (int i = 0; i < nRevs; ++i) {
            optional<Revision> rev = remoteRevision(RemoteID(i));
            if (rev) {
                if (i > 0)
                    out << "; R" << i << '@';
                if (rev->revID)
                    out << rev->revID.str();
                else
                    out << "--";
                if (rev->flags != DocumentFlags::kNone) {
                    out << "(";
                    if (rev->isDeleted()) out << "D";
                    if (rev->isConflicted()) out << "C";
                    if (rev->hasAttachments()) out << "A";
                    out << ')';
                }
            }
        }
    }

    string NuDocument::dump() const {
        stringstream out;
        dump(out);
        return out.str();
    }

}


#pragma mark - INTERNALS:


// This stuff is kept below because the rest because it uses the Fleece "impl" API,
// and having both APIs in scope gets confusing and leads to weird compiler errors.

#include "Doc.hh"  // fleece::impl::Doc


namespace litecore {

    // Subclass of Doc that points back to the NuDocument instance. That way when we use
    // Scope::containing to look up where a Fleece Value* is, we can track it back to the
    // NuDocument that owns the Doc.
    class NuDocument::LinkedFleeceDoc : public fleece::impl::Doc {
    public:
        LinkedFleeceDoc(const alloc_slice &fleeceData, fleece::impl::SharedKeys* sk,
                        NuDocument *document_)
        :fleece::impl::Doc(fleeceData, Doc::kTrusted, sk)
        ,document(document_)
        { }

        NuDocument* const document;
    };


    bool NuDocument::initFleeceDoc(const alloc_slice &body) {
        auto sk = _store.dataFile().documentKeys();
        _sharedKeys = FLSharedKeys(sk);
        if (body) {
            _fleeceDoc = new LinkedFleeceDoc(body, sk, this);
            _revisions = (FLArray)_fleeceDoc->asArray();
            _mutatedRevisions = nullptr;
            return true;
        } else {
            return false;
        }
    }


    Array NuDocument::savedRevisions() const {
        return _fleeceDoc ? (FLArray)_fleeceDoc->asArray() : nullptr;
    }


    NuDocument* NuDocument::containing(Value value) {
        if (value.isMutable()) {
            // Scope doesn't know about mutable Values (they're in the heap), but the mutable
            // Value may be a mutable copy of a Value with scope...
            if (value.asDict())
                value = value.asDict().asMutable().source();
            else
                value = value.asArray().asMutable().source();
            if (!value)
                return nullptr;
        }

        const impl::Scope *scope = impl::Scope::containing((const impl::Value*)(FLValue)value);
        if (!scope)
            return nullptr;
        auto versScope = dynamic_cast<const LinkedFleeceDoc*>(scope);
        if (!versScope)
            return nullptr;
        return versScope->document;
    }


    string NuDocument::dumpStorage() const {
        return _fleeceDoc ? fleece::impl::Value::dump(_fleeceDoc->allocedData()) : "";
    }

}
