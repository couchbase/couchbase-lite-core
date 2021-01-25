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
#include "VersionVector.hh"
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
     RECORD SCHEMA:

     A table row, and `Record` object, contain these columns/properties:
       - `key`        --The document ID
       - `version`    --Current revision's ID (if a version, just 1st component, not whole vector)
       - `flags`      --Current document flags, based on all stored revisions
       - `sequence`   --The document's current sequence number
       - `body`       --Fleece-encoded properties of the current revision
       - `extra`      --Other revisions, if any, as described below

     This separation of `body` and `extra` lets us avoid reading the remote revision(s) into RAM
     unless needed ... and they're normally only needed by the replicator.

     THE "EXTRA" COLUMN:

     If remote revisions are stored, the `extra` column contains a Fleece-encoded Array.

     The indices of the Array correspond to `RemoteID`s. Each remote revision is stored at its
     RemoteID's index as a Dict, with keys:
       - `kRevPropertiesKey` --document body, itself a Dict
       - `kRevIDKey`         --revision ID, binary data
       - `kRevFlagsKey`      --DocumentFlags, int, omitted if 0
     An array item whose index doesn't correspond to any Revision contains a `null` instead
     of a Dict. This includes the first (0) item, since storing the local revision here would be
     redundant.

     DE-DUPLICATING PROPERTY VALUES:

     It's very common for two or more RemoteIDs to refer to the same revision, i.e. have the same
     version/properties/flags. This happens whenever the local document is in sync with its remote
     counterpart.

     It's also common for different revisions to have a lot of common property values; for example,
     if the local database changes one property but leaves the rest alone.

     Thus, when encoding the record `body` and `extra` we use a DeDuplicatingEncoder to save
     space. This encoder recognizes when `writeValue` is called twice with the same `Value`;
     after the first time it just encodes a Fleece "pointer" to the already-encoded value data.
     (This turns the Fleece structure, normally a tree, into a DAG. That's largely immaterial to
     clients, because the structure is read-only. You'd have to be looking for equal pointers to
     tell the difference.)

     Even this wouldn't normally de-duplicate between the _current_ revision and a remote,
     since they're encoded into separate Fleece containers (stored in `body` and `extra`.)
     To get around that, we use the arcane `FLEncoder_Snip` function, which allows you to write
     multiple Fleece containers with the same encoder. We write the body properties first, snip
     those as one container that will be written to `body`, then continue encoding the rest of
     the remote revisions, which will end up in `extra`. This means that `extra` may contain
     references back into `body`, but this is OK as long as, when we load `extra`, we tell it that
     its "extern" data is the `body` data. Then, when Fleece detects it's following an internal
     reference in `extra` whose destination is outside `extra`, it will resolve it to the
     corresponding address in `body`. It's as though they're a single container.
     */

    
    // Keys in revision dicts (deliberately tiny and ineligible for SharedKeys, to save space.)
    static constexpr slice kRevPropertiesKey = ".";
    static constexpr slice kRevIDKey         = "@";
    static constexpr slice kRevFlagsKey      = "&";


    Version Revision::version() const {
        return VersionVector::readCurrentVersionFromBinary(revID);
    }

    VersionVector Revision::versionVector() const {
        return VersionVector::fromBinary(revID);
    }



    NuDocument::NuDocument(KeyStore& store, Versioning versioning, const Record& rec)
    :_store(store)
    ,_docID(rec.key())
    ,_sequence(rec.sequence())
    ,_revID(rec.version())
    ,_docFlags(rec.flags())
    ,_whichContent(rec.contentLoaded())
    ,_versioning(versioning)
    {
        _current.revID = revid(_revID);
        _current.flags = rec.flags() - DocumentFlags::kConflicted - DocumentFlags::kSynced;
        if (rec.exists()) {
            readRecordBody(rec.body());
            readRecordExtra(rec.extra());
        } else {
            // ‘"Untitled" empty state. Create an empty local properties dict:
            _whichContent = kEntireBody;
            (void)mutableProperties();
        }
    }


    NuDocument::NuDocument(KeyStore& store, Versioning v, slice docID, ContentOption whichContent)
    :NuDocument(store, v, store.get(docID, whichContent))
    { }


    NuDocument::~NuDocument() = default;


    void NuDocument::readRecordBody(const alloc_slice &body) {
        if (body) {
            _bodyDoc = newLinkedFleeceDoc(body);
            _current.properties = _bodyDoc.asDict();
            if (!_current.properties)
                error::_throw(error::CorruptRevisionData);
        } else  {
            _bodyDoc = nullptr;
            if (_whichContent != kMetaOnly)
                _current.properties = Dict::emptyDict();
            else
                _current.properties = nullptr;
        }
        _currentProperties = _current.properties;       // retains it
    }

    void NuDocument::readRecordExtra(const alloc_slice &extra) {
        if (extra) {
            _extraDoc = Doc(extra, kFLTrusted, sharedKeys(), _bodyDoc.data());
        }
        else
            _extraDoc = nullptr;
        _revisions = _extraDoc.asArray();
        _mutatedRevisions = nullptr;
        if (extra && !_revisions)
            error::_throw(error::CorruptRevisionData);

        // The kSynced flag is set when the document's current revision is pushed to remote #1.
        // This is done instead of updating the doc body, for reasons of speed. So when loading
        // the document, detect that flag and belatedly update remote #1's state.
        if (_docFlags & DocumentFlags::kSynced) {
            setRemoteRevision(RemoteID(1), currentRevision());
            _docFlags = _docFlags - DocumentFlags::kSynced;
            _changed = false;
        }
    }


    bool NuDocument::loadData(ContentOption which) {
        if (!exists())
            return false;
        if (which <= _whichContent)
            return true;
        
        Record rec = _store.get(_sequence, which);
        if (!rec.exists())
            return false;

        //Warn("NuDocument: Loading more data (which=%d) of '%.*s'", int(which), SPLAT(docID()));
        auto oldWhich = _whichContent;
        _whichContent = which;
        if (which >= kCurrentRevOnly && oldWhich < kCurrentRevOnly)
            readRecordBody(rec.body());
        if (which == kEntireBody && oldWhich < kEntireBody)
            readRecordExtra(rec.extra());
        return true;
    }


    void NuDocument::requireBody() const {
        if (_whichContent < kCurrentRevOnly)
            error::_throw(error::UnsupportedOperation, "Document's body is not loaded");
    }

    void NuDocument::requireRemotes() const {
        if (_whichContent < kEntireBody)
            error::_throw(error::UnsupportedOperation, "Document's other revisions are not loaded");
    }


    void NuDocument::mustLoadRemotes() {
        if (exists() && !loadData(kEntireBody))
            error::_throw(error::Conflict, "Document is outdated, revisions can't be loaded");
    }


#pragma mark - REVISIONS:


    optional<Revision> NuDocument::remoteRevision(RemoteID remote) const {
        if (remote == RemoteID::Local)
            return currentRevision();
        requireRemotes();

        if (Dict revDict = _revisions[int(remote)].asDict(); revDict) {
            // revisions have a top-level dict with the revID, flags, properties.
            Dict properties = revDict[kRevPropertiesKey].asDict();
            revid revID(revDict[kRevIDKey].asData());
            auto flags = DocumentFlags(revDict[kRevFlagsKey].asInt());
            if (!properties)
                properties = Dict::emptyDict();
            if (!revID)
                error::_throw(error::CorruptRevisionData);
            return Revision{properties, revID, flags};
        } else {
            return nullopt;
        }
    }


    optional<Revision> NuDocument::loadRemoteRevision(RemoteID remote) {
        if (remote != RemoteID::Local)
            mustLoadRemotes();
        return remoteRevision(remote);
    }


    RemoteID NuDocument::nextRemoteID(RemoteID remote) const {
        int iremote = int(remote);
        while (++iremote < _revisions.count()) {
            if (_revisions[iremote].asDict() != nullptr)
                break;
        }
        return RemoteID(iremote);
    }


    RemoteID NuDocument::loadNextRemoteID(RemoteID remote) {
        mustLoadRemotes();
        return nextRemoteID(remote);
    }

    // If _revisions is not mutable, makes a mutable copy and assigns it to _mutatedRevisions.
    void NuDocument::mutateRevisions() {
        requireRemotes();
        if (!_mutatedRevisions) {
            _mutatedRevisions = _revisions ? _revisions.mutableCopy() : MutableArray::newArray();
            _revisions = _mutatedRevisions;
        }
    }

    // Returns the MutableDict for a revision.
    // If it's not mutable yet, replaces it with a mutable copy.
    MutableDict NuDocument::mutableRevisionDict(RemoteID remote) {
        Assert(remote > RemoteID::Local);
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
        if (remote == RemoteID::Local) {
            Assert(optRev);
            return setCurrentRevision(*optRev);
        }

        mustLoadRemotes();
        bool changedFlags = false;
        if (auto &newRev = *optRev; optRev) {
            // Creating/updating a remote revision:
            MutableDict revDict = mutableRevisionDict(remote);
            if (!newRev.revID)
                error::_throw(error::CorruptRevisionData);
            if (auto oldRevID = revDict[kRevIDKey].asData(); newRev.revID != oldRevID) {
                revDict[kRevIDKey].setData(newRev.revID);
                _changed = true;
            }
            if (newRev.properties != revDict[kRevPropertiesKey]) {
                if (newRev.properties)
                    revDict[kRevPropertiesKey] = newRev.properties;
                else
                    revDict.remove(kRevPropertiesKey);
                _changed = true;
            }
            if (int(newRev.flags) != revDict[kRevFlagsKey].asInt()) {
                if (newRev.flags != DocumentFlags::kNone)
                    revDict[kRevFlagsKey] = int(newRev.flags);
                else
                    revDict.remove(kRevFlagsKey);
                _changed = changedFlags = true;
            }
        } else if (_revisions[int(remote)]) {
            // Removing a remote revision.
            // First replace its Dict with null, then remove trailing nulls from the revision array.
            mutateRevisions();
            _mutatedRevisions[int(remote)] = Value::null();
            auto n = _mutatedRevisions.count();
            while (n > 0 && !_mutatedRevisions[n-1].asDict())
                --n;
            _mutatedRevisions.resize(n);
            _changed = changedFlags = true;
        }

        if (changedFlags)
            updateDocFlags();
    }


#pragma mark - CURRENT REVISION:


    slice NuDocument::currentRevisionData() const {
        requireBody();
        return _bodyDoc.data();
    }


    void NuDocument::setCurrentRevision(const Revision &rev) {
        setRevID(rev.revID);
        setProperties(rev.properties);
        setFlags(rev.flags);
    }


    Dict NuDocument::originalProperties() const {
        requireBody();
        return _bodyDoc.asDict();
    }


    MutableDict NuDocument::mutableProperties() {
        requireBody();
        MutableDict mutProperties = _current.properties.asMutable();
        if (!mutProperties) {
            // Make a mutable copy of the current properties:
            mutProperties = _current.properties.mutableCopy();
            if (!mutProperties)
                mutProperties = MutableDict::newDict();
            _current.properties = mutProperties;
            _currentProperties = mutProperties;
        }
        return mutProperties;
    }


    void NuDocument::setProperties(Dict newProperties) {
        requireBody();
        if (newProperties != _current.properties) {
            _currentProperties = newProperties;
            _current.properties = newProperties;
            _changed = true;
        }
    }

    void NuDocument::setRevID(revid newRevID) {
        requireBody();
        if (!newRevID)
            error::_throw(error::InvalidParameter);
        if (newRevID != _current.revID) {
            _revID = alloc_slice(newRevID);
            _current.revID = revid(_revID);
            _changed = _revIDChanged = true;
        }
    }

    void NuDocument::setFlags(DocumentFlags newFlags) {
        requireBody();
        if (newFlags != _current.flags) {
            _current.flags = newFlags;
            _changed = true;
            updateDocFlags();
        }
    }


#pragma mark - CHANGE HANDLING:


    void NuDocument::updateDocFlags() {
        // Take the local revision's flags, and add the Conflicted and Attachments flags
        // if any remote rev has them.
        auto newDocFlags = _docFlags - DocumentFlags::kConflicted - DocumentFlags::kHasAttachments;
        newDocFlags = newDocFlags | _current.flags;
        for (Array::iterator i(_revisions); i; ++i) {
            Dict revDict = i.value().asDict();
            if (revDict) {
                auto flags = DocumentFlags(revDict[kRevFlagsKey].asInt());
                if (flags & DocumentFlags::kConflicted)
                    newDocFlags |= DocumentFlags::kConflicted;
                if (flags & DocumentFlags::kHasAttachments)
                    newDocFlags |= DocumentFlags::kHasAttachments;
            }
        }
        _docFlags = newDocFlags;
    }


    bool NuDocument::changed() const {
        return _changed || propertiesChanged();
    }


    bool NuDocument::propertiesChanged() const {
        for (DeepIterator i(_current.properties); i; ++i) {
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
        for (DeepIterator i(_current.properties); i; ++i) {
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
        requireRemotes();
        auto [_, revID, flags] = currentRevision();
        bool newRevision = !revID || propertiesChanged();
        if (!newRevision && !_changed)
            return kNoSave;

        // If the revID hasn't been changed but the local properties have, generate a new revID:
        alloc_slice generatedRev;
        if (newRevision && !_revIDChanged) {
            switch (_versioning) {
                case Versioning::RevTrees:
                    generatedRev = generateRevID(_current.properties, revID, flags);
                    break;
                case Versioning::Vectors:
                    generatedRev = generateVersionVector(revID);
            }
            revID = revid(generatedRev);
            setRevID(revID);
            Log("Generated revID '%s'", revID.str().c_str());
        }

        alloc_slice body, extra;
        tie(body, extra) = encodeBodyAndExtra();

        auto seq = _sequence;
        bool updateSequence = (seq == 0 || _revIDChanged);
        Assert(revID);
        RecordLite rec = {_docID, revID, body, extra, seq, updateSequence, _docFlags};
        seq = _store.set(rec, transaction);
        if (seq == 0)
            return kConflict;

        _sequence = seq;
        _changed = _revIDChanged = false;

        // Update Fleece Doc to newly saved data:
        MutableDict mutableProperties = _current.properties.asMutable();
        readRecordBody(body);
        readRecordExtra(extra);
        if (mutableProperties) {
            // The client might still have references to mutable objects under _properties,
            // so keep that mutable Dict as the current _properties:
            _current.properties = mutableProperties;
            _currentProperties = mutableProperties;
            clearPropertiesChanged();
        }

        return updateSequence ? kNewSequence : kNoNewSequence;
    }


    pair<alloc_slice,alloc_slice> NuDocument::encodeBodyAndExtra() {
        return _encoder ? encodeBodyAndExtra(_encoder)
                        : encodeBodyAndExtra(Encoder(sharedKeys()));
    }


    pair<alloc_slice,alloc_slice> NuDocument::encodeBodyAndExtra(FLEncoder flEnc) {
        SharedEncoder enc(flEnc);
        alloc_slice body, extra;
        unsigned nRevs = _revisions.count();
        if (nRevs == 0) {
            // Only a current rev, nothing else, so only generate a body:
            if (!_current.properties.empty()) {
                enc.writeValue(_current.properties);
                body = enc.finish();
            }
        } else {
            enc.beginArray();
            DeDuplicateEncoder ddenc(enc);
            // Write current rev:
            enc.beginDict();
            enc.writeKey(kRevPropertiesKey);
            ddenc.writeValue(_current.properties, 1);
            body = alloc_slice(FLEncoder_Snip(enc));
            enc.endDict();

            // Write other revs:
            for (unsigned i = 1; i < nRevs; i++) {
                Value rev = _revisions[i];
                ddenc.writeValue(rev, 2);
            }
            enc.endArray();
            extra = enc.finish();
        }

        return {body, extra};
    }


    alloc_slice NuDocument::generateRevID(Dict body, revid parentRevID, DocumentFlags flags) {
        // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and JSON:
        alloc_slice json = FLValue_ToJSONX(body, false, true);
        parentRevID.setSize(min(parentRevID.size, size_t(255)));
        uint8_t revLen = (uint8_t)parentRevID.size;
        uint8_t delByte = (flags & DocumentFlags::kDeleted) != 0;
        SHA1 digest = (SHA1Builder() << revLen << parentRevID << delByte << json).finish();
        unsigned generation = parentRevID ? parentRevID.generation() + 1 : 1;
        return alloc_slice(revidBuffer(generation, slice(digest)));
    }


    alloc_slice NuDocument::generateVersionVector(revid parentRevID) {
        VersionVector vec = parentRevID.asVersionVector();
        vec.incrementGen(kMePeerID);
        return vec.asBinary();
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


    FLSharedKeys NuDocument::sharedKeys() const {
        return (FLSharedKeys)_store.dataFile().documentKeys();
    }


    Doc NuDocument::newLinkedFleeceDoc(const alloc_slice &body) {
        auto sk = _store.dataFile().documentKeys();
        return (FLDoc) new LinkedFleeceDoc(body, sk, this);
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
        stringstream out;
        if (_bodyDoc) {
            slice data = _bodyDoc.allocedData();
            out << "---BODY: " << data.size << " bytes at " << (void*)data.buf << ":\n";
            fleece::impl::Value::dump(data, out);
        }
        if (_extraDoc) {
            slice data = _extraDoc.allocedData();
            out << "---EXTRA: " << data.size << " bytes at " << (void*)data.buf << ":\n";
            fleece::impl::Value::dump(data, out);
        }
        return out.str();
    }


    /*static*/ void NuDocument::forAllRevIDs(const RecordLite &rec,
                                             function_ref<void(revid,RemoteID)> callback)
    {
        callback(revid(rec.version), RemoteID::Local);
        if (rec.extra.size > 0) {
            fleece::impl::Scope scope(rec.extra, nullptr, rec.body);
            Array remotes = Value::fromData(rec.extra, kFLTrusted).asArray();
            int n = 0;
            for (Array::iterator i(remotes); i; ++i, ++n) {
                if (n > 0) {
                    Dict remote = i.value().asDict();
                    slice revID = remote[kRevIDKey].asData();
                    if (revID)
                        callback(revid(revID), RemoteID(n));
                }
            }
        }
    }

}
