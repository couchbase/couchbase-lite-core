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


    NuDocument::NuDocument(KeyStore& store, slice docID)
    :NuDocument(store, store.get(docID))
    { }


    NuDocument::NuDocument(KeyStore& store, const Record& rec)
    :_store(store)
    ,_docID(rec.key())
    ,_sequence(rec.sequence())
    {
        decode(rec.body());
    }


    NuDocument::~NuDocument() = default;


    void NuDocument::decode(const alloc_slice &body) {
        if (initFleeceDoc(body)) {
            if (!_revisions)
                error::_throw(error::CorruptRevisionData);
            _properties = _revisions[kLocal].asDict()[kMetaBody].asDict();
            if (!_properties)
                error::_throw(error::CorruptRevisionData);
        } else {
            // "Untitled" empty state:
            (void)mutableProperties();
        }
    }


#pragma mark - REVISIONS:


    optional<Revision> NuDocument::remoteRevision(RemoteID remote) const {
        if (Dict revDict = _revisions[remote].asDict(); revDict) {
            // revisions have a top-level dict with the revID, flags, properties.
            Dict properties = revDict[kMetaBody].asDict();
            revid revID(revDict[kMetaRevID].asData());
            auto flags = DocumentFlags(revDict[kMetaFlags].asInt());
            if (remote == kLocal) {
                if (!properties)
                    properties = Dict::emptyDict();
            } else {
                if (!properties || !revID)
                    error::_throw(error::CorruptRevisionData);
            }
            return Revision{properties, revID, flags};
        } else {
            return nullopt;
        }
    }


    void NuDocument::mutateRevisions() {
        if (!_mutatedRevisions) {
            _mutatedRevisions = _revisions ? _revisions.mutableCopy() : MutableArray::newArray();
            _revisions = _mutatedRevisions;
        }
    }

    MutableDict NuDocument::mutableRevisionDict(RemoteID remoteID) {
        mutateRevisions();
        if (_mutatedRevisions.count() <= remoteID)
            _mutatedRevisions.resize(remoteID + 1);
        MutableDict revDict = _mutatedRevisions.getMutableDict(remoteID);
        if (!revDict)
            _mutatedRevisions[remoteID] = revDict = MutableDict::newDict();
        return revDict;
    }


    void NuDocument::setRemoteRevision(RemoteID remoteID, const optional<Revision> &optRev) {
        if (!optRev && !_revisions[remoteID]) {
            return;
        } else if (auto &newRev = *optRev; optRev) {
            // Creating/updating a revision:
            Assert(newRev.properties);
            Assert(newRev.revID || remoteID == kLocal);
            MutableDict revDict = mutableRevisionDict(remoteID);
            revDict[kMetaBody] = newRev.properties;
            if (newRev.revID != revDict[kMetaRevID].asData()) {
                revDict[kMetaRevID].setData(newRev.revID);
                if (remoteID == kLocal)
                    _revIDChanged = true;
            }
            if (newRev.flags != DocumentFlags::kNone)
                revDict[kMetaFlags] = int(newRev.flags);
            else
                revDict.remove(kMetaFlags);
        } else {
            // Removing a remote revision:
            Assert(remoteID != kLocal);
            mutateRevisions();
            _mutatedRevisions[remoteID] = Value::null();
            auto n = _mutatedRevisions.count();
            while (n > 0 && !_mutatedRevisions[n-1].asDict())
                --n;
            _mutatedRevisions.resize(n);
        }
        _changed = true;
    }


#pragma mark - CURRENT REVISION:


    Revision NuDocument::currentRevision() const {
        return *remoteRevision(kLocal);
    }


    void NuDocument::setCurrentRevision(const Revision &newRev) {
        setRemoteRevision(kLocal, newRev);
    }


    revid NuDocument::revID() const {
        return currentRevision().revID;
    }


    DocumentFlags NuDocument::flags() const {
        return currentRevision().flags;
    }


    void NuDocument::setRevID(revid newRevID) {
        Revision rev = currentRevision();
        if (newRevID != rev.revID)
            setCurrentRevision({rev.properties, newRevID, rev.flags});
    }


    void NuDocument::setFlags(DocumentFlags newFlags) {
        Revision rev = currentRevision();
        if (newFlags != rev.flags)
            setCurrentRevision({rev.properties, rev.revID, newFlags});
    }


#pragma mark - DOCUMENT ROOT/PROPERTIES:


    MutableDict NuDocument::mutableProperties() {
        MutableDict mutProperties = _properties.asMutable();
        if (!mutProperties) {
            MutableDict rev = mutableRevisionDict(kLocal);
            mutProperties = rev.getMutableDict(kMetaBody);
            if (!mutProperties) {
                mutProperties = MutableDict::newDict();
                rev[kMetaBody] = mutProperties;
            }
            _properties = mutProperties;
        }
        return mutProperties;
    }


    void NuDocument::setProperties(Dict properties) {
        if (properties == _properties)
            return;
        MutableDict rev = mutableRevisionDict(kLocal);
        rev[kMetaBody] = properties;
        _properties = properties;
        _changed = true;
    }


    bool NuDocument::changed() const {
        return _changed || _properties.asMutable().isChanged();
    }


#pragma mark - SAVING:


    NuDocument::SaveResult NuDocument::save(Transaction &t, revid revID, DocumentFlags flags) {
        Assert(revID);
        auto cur = currentRevision();
        if (revID != cur.revID || flags != cur.flags)
            setCurrentRevision({cur.properties, revID, flags});
        return save(t);
    }


    NuDocument::SaveResult NuDocument::save(Transaction& transaction) {
        if (!changed())
            return kNoSave;

        auto [_, revID, flags] = currentRevision();

        // If the revID hasn't been changed but the local properties have, generate a new revID:
        revidBuffer generatedRev;
        if (!_revIDChanged && (!revID || _properties.asMutable().isChanged())) {
            generatedRev = generateRevID(_properties, revID, flags);
            setRevID(generatedRev);
            revID = generatedRev;
            Log("Generated revID '%s'", generatedRev.str().c_str());
        }

        alloc_slice body = encodeBody();

        auto seq = _sequence;
        bool newSequence = (seq == 0 || _revIDChanged);
        seq = _store.set(_docID, revID, body, flags, transaction, seq, newSequence);
        if (seq == 0)
            return kConflict;

        _sequence = seq;

        // Go back to unchanged state, by reloading from the Record:
        _changed = _revIDChanged = false;
        _fleeceDoc = nullptr;
        _revisions = nullptr;
        _mutatedRevisions = nullptr;
        _properties = nullptr;
        decode(body);
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


    alloc_slice NuDocument::encodeBody() {
        if (!_properties)
            return nullslice;
        else if (_encoder)
            return encodeBody(_encoder);
        else
            return encodeBody(Encoder(_sharedKeys));
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
            optional<Revision> rev = remoteRevision(i);
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
            return true;
        } else {
            return false;
        }
    }


    alloc_slice NuDocument::docBody() const {
        return _fleeceDoc->allocedData();
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
        return fleece::impl::Value::dump(docBody());
    }

}
