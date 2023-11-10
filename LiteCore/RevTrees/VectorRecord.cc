//
// VectorRecord.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "VectorRecord.hh"
#include "Record.hh"
#include "KeyStore.hh"
#include "DataFile.hh"
#include "VersionVector.hh"
#include "DeDuplicateEncoder.hh"
#include "Error.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include "RawRevTree.hh"
#include "RevTree.hh"
#include "fleece/Expert.hh"
#include <ostream>
#include <sstream>

namespace litecore {
    using namespace std;
    using namespace fleece;

    /*
     RECORD SCHEMA:

     A table row, and `Record` object, contain these columns/properties:
       - `key`        --The document ID
       - `version`    --Current revision's ID (entire version vector, or current tree revID)
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
    static constexpr slice kLegacyRevIDKey   = "-";
    static constexpr slice kRevFlagsKey      = "&";

    bool Revision::hasVersionVector() const { return revID.isVersion(); }

    Version Revision::version() const { return VersionVector::readCurrentVersionFromBinary(revID); }

    VersionVector Revision::versionVector() const { return VersionVector::fromBinary(revID); }

    VectorRecord::VectorRecord(KeyStore& store, const Record& rec)
        : _store(store)
        , _docID(rec.key())
        , _sequence(rec.sequence())
        , _subsequence(rec.subsequence())
        , _savedRevID(rec.version())
        , _revID(_savedRevID)
        , _docFlags(rec.flags())
        , _whichContent(rec.contentLoaded()) {
        _current.revID = revid(_revID);
        _current.flags = _docFlags - (DocumentFlags::kConflicted | DocumentFlags::kSynced);
        if ( rec.exists() ) {
            readRecordBody(rec.body());
            readRecordExtra(rec.extra());
        } else {
            // ‘"Untitled" empty state. Create an empty local properties dict:
            _sequence     = 0_seq;
            _whichContent = kEntireBody;
            (void)mutableProperties();
        }
    }

    VectorRecord::VectorRecord(KeyStore& store, slice docID, ContentOption whichContent)
        : VectorRecord(store, store.get(docID, whichContent)) {}

    VectorRecord::VectorRecord(const VectorRecord& other) : VectorRecord(other._store, other.originalRecord()) {}

    VectorRecord::~VectorRecord() = default;

    void VectorRecord::readRecordBody(const alloc_slice& body) {
        if ( body && !revid(_revID).isVersion() && RawRevision::isRevTree(body) ) {
            // doc is still in v2.x format, with body & rev-tree in `body`, and no `extra`:
            importRevTree(body, nullslice);
        } else {
            if ( body ) {
                _bodyDoc            = newLinkedFleeceDoc(body, kFLTrusted);
                _current.properties = _bodyDoc.asDict();
                if ( !_current.properties )
                    error::_throw(error::CorruptRevisionData, "VectorRecord reading properties error");
            } else {
                _bodyDoc = nullptr;
                if ( _whichContent != kMetaOnly ) _current.properties = Dict::emptyDict();
                else
                    _current.properties = nullptr;
            }
            _currentProperties = _current.properties;  // retains it
        }
    }

    void VectorRecord::readRecordExtra(const alloc_slice& extra) {
        if ( extra && !revid(_revID).isVersion() ) {
            // This doc hasn't been upgraded; `extra` is still in old RevTree format
            importRevTree(_bodyDoc.allocedData(), extra);
        } else {
            if ( extra ) {
                _extraDoc = Doc(extra, kFLTrusted, sharedKeys(), _bodyDoc.data());
            } else
                _extraDoc = nullptr;
            _revisions        = _extraDoc.asArray();
            _mutatedRevisions = nullptr;
            if ( extra && !_revisions ) error::_throw(error::CorruptRevisionData, "VectorRecord readRecordExtra error");
        }

        // The kSynced flag is set when the document's current revision is pushed to remote #1.
        // This is done instead of updating the doc body, for reasons of speed. So when loading
        // the document, detect that flag and belatedly update remote #1's state.
        if ( _docFlags & DocumentFlags::kSynced ) {
            setRemoteRevision(RemoteID(1), currentRevision());
            _docFlags -= DocumentFlags::kSynced;
            _changed = false;
        }
    }

    // Parses `extra` column as an old-style RevTree and adds the revisions.
    void VectorRecord::importRevTree(alloc_slice body, alloc_slice extra) {
        LogToAt(DBLog, Verbose, "VectorRecord: importing '%.*s' as RevTree", SPLAT(docID()));
        bool wasChanged = _changed;
        _extraDoc       = fleece::Doc(extra, kFLTrustedDontParse, sharedKeys());
        RevTree    revTree(body, extra, sequence());
        const Rev* curRev = revTree.currentRevision();
        if ( _docFlags & DocumentFlags::kSynced ) revTree.setLatestRevisionOnRemote(1, curRev);

        if ( !extra ) {
            // This is a v2.x document with body & rev-tree in `body`, and no `extra`:
            Assert(!_bodyDoc);
            _bodyDoc            = newLinkedFleeceDoc(body, kFLTrustedDontParse);
            FLValue bodyProps   = FLValue_FromData(curRev->body(), kFLTrusted);
            _current.properties = Value(bodyProps).asDict();
            if ( !_current.properties )
                error::_throw(error::CorruptRevisionData, "VectorRecord reading 2.x properties error");
            _currentProperties = _current.properties;  // retains it
        }

        // Propagate any saved remote revisions to the new document:
        auto& remoteRevMap = revTree.remoteRevisions();
        for ( auto [id, rev] : remoteRevMap ) {
            Revision    nuRev;
            MutableDict nuProps;
            if ( rev == curRev ) {
                nuRev = currentRevision();
            } else {
                if ( rev->body() ) {
                    auto props       = fleece::ValueFromData(rev->body(), kFLTrusted).asDict();
                    nuProps          = props.mutableCopy(kFLDeepCopyImmutables);
                    nuRev.properties = nuProps;
                }
                nuRev.revID = rev->revID;
                nuRev.flags = {};
                if ( rev->flags & Rev::kDeleted ) nuRev.flags |= DocumentFlags::kDeleted;
                if ( rev->flags & Rev::kHasAttachments ) nuRev.flags |= DocumentFlags::kHasAttachments;
                if ( rev->flags & Rev::kIsConflict ) nuRev.flags |= DocumentFlags::kConflicted;
            }
            setRemoteRevision(RemoteID(id), nuRev);
        }

        _changed = wasChanged;
    }

    bool VectorRecord::loadData(ContentOption which) {
        if ( !exists() ) return false;
        if ( which <= _whichContent ) return true;

        Record rec = _store.get(_sequence, which);
        if ( !rec.exists() ) return false;

        LogToAt(DBLog, Verbose, "VectorRecord: Loading more data (which=%d) of '%.*s'", int(which), SPLAT(docID()));
        auto oldWhich = _whichContent;
        _whichContent = which;
        if ( which >= kCurrentRevOnly && oldWhich < kCurrentRevOnly ) readRecordBody(rec.body());
        if ( which >= kEntireBody && oldWhich < kEntireBody ) readRecordExtra(rec.extra());
        return true;
    }

    // Reconstitutes the original Record I was loaded from
    Record VectorRecord::originalRecord() const {
        Record rec(_docID);
        rec.updateSequence(_sequence);
        rec.updateSubsequence(_subsequence);
        if ( _sequence > 0_seq ) rec.setExists();
        rec.setVersion(_savedRevID);
        rec.setFlags(_docFlags);
        rec.setBody(_bodyDoc.allocedData());
        rec.setExtra(_extraDoc.allocedData());
        rec.setContentLoaded(_whichContent);
        return rec;
    }

    void VectorRecord::requireBody() const {
        if ( _whichContent < kCurrentRevOnly )
            error::_throw(error::UnsupportedOperation, "Document's body is not loaded");
    }

    void VectorRecord::requireRemotes() const {
        if ( _whichContent < kEntireBody )
            error::_throw(error::UnsupportedOperation, "Document's other revisions are not loaded");
    }

    void VectorRecord::mustLoadRemotes() {
        if ( exists() && !loadData(kEntireBody) )
            error::_throw(error::Conflict, "Document is outdated, revisions can't be loaded");
    }

#pragma mark - REVISIONS:

    optional<Revision> VectorRecord::remoteRevision(RemoteID remote) const {
        if ( remote == RemoteID::Local ) return currentRevision();
        requireRemotes();

        if ( Dict revDict = _revisions[int(remote)].asDict(); revDict ) {
            // revisions have a top-level dict with the revID, flags, properties.
            Dict  properties = revDict[kRevPropertiesKey].asDict();
            revid revID(revDict[kRevIDKey].asData());
            auto  flags = DocumentFlags(revDict[kRevFlagsKey].asInt());
            if ( !properties ) properties = Dict::emptyDict();
            if ( !revID ) error::_throw(error::CorruptRevisionData, "VectorRecord remoteRevision bad revID");
            return Revision{properties, revID, flags};
        } else {
            return nullopt;
        }
    }

    optional<Revision> VectorRecord::loadRemoteRevision(RemoteID remote) {
        if ( remote != RemoteID::Local ) mustLoadRemotes();
        return remoteRevision(remote);
    }

    RemoteID VectorRecord::nextRemoteID(RemoteID remote) const {
        int iremote = int(remote);
        while ( ++iremote < _revisions.count() ) {
            if ( _revisions[iremote].asDict() != nullptr ) break;
        }
        return RemoteID(iremote);
    }

    RemoteID VectorRecord::loadNextRemoteID(RemoteID remote) {
        mustLoadRemotes();
        return nextRemoteID(remote);
    }

    void VectorRecord::forAllRevs(const ForAllRevsCallback& callback) const {
        RemoteID           rem = RemoteID::Local;
        optional<Revision> rev;
        while ( (rev = remoteRevision(rem)) ) {
            callback(rem, *rev);
            rem = nextRemoteID(rem);
        }
    }

    // If _revisions is not mutable, makes a mutable copy and assigns it to _mutatedRevisions.
    void VectorRecord::mutateRevisions() {
        requireRemotes();
        if ( !_mutatedRevisions ) {
            _mutatedRevisions = _revisions ? _revisions.mutableCopy() : MutableArray::newArray();
            _revisions        = _mutatedRevisions;
        }
    }

    // Returns the MutableDict for a revision.
    // If it's not mutable yet, replaces it with a mutable copy.
    MutableDict VectorRecord::mutableRevisionDict(RemoteID remote) {
        mutateRevisions();
        if ( _mutatedRevisions.count() <= int(remote) ) _mutatedRevisions.resize(int(remote) + 1);
        MutableDict revDict = _mutatedRevisions.getMutableDict(int(remote));
        if ( !revDict ) _mutatedRevisions[int(remote)] = revDict = MutableDict::newDict();
        return revDict;
    }

    // Updates a revision. (Local changes, e.g. setRevID and setFlags, go through this too.)
    void VectorRecord::setRemoteRevision(RemoteID remote, const optional<Revision>& optRev) {
        if ( remote == RemoteID::Local ) {
            Assert(optRev);
            return setCurrentRevision(*optRev);
        }

        mustLoadRemotes();
        bool changedFlags = false;
        if ( auto& newRev = *optRev; optRev ) {
            // Creating/updating a remote revision:
            Assert((uint8_t(newRev.flags) & ~0x7) == 0);  // only deleted/attachments/conflicted are legal
            MutableDict revDict = mutableRevisionDict(remote);
            if ( !newRev.revID ) error::_throw(error::CorruptRevisionData, "VectorRecord setRemoteRevision bad revID");
            if ( auto oldRevID = revDict[kRevIDKey].asData(); newRev.revID != oldRevID ) {
                revDict[kRevIDKey].setData(newRev.revID);
                _changed = true;
            }
            if ( newRev.properties != revDict.get(kRevPropertiesKey) ) {
                if ( newRev.properties ) revDict[kRevPropertiesKey] = newRev.properties;
                else
                    revDict.remove(kRevPropertiesKey);
                _changed = true;
            }
            if ( int(newRev.flags) != revDict[kRevFlagsKey].asInt() ) {
                if ( newRev.flags != DocumentFlags::kNone ) revDict[kRevFlagsKey] = int(newRev.flags);
                else
                    revDict.remove(kRevFlagsKey);
                _changed = changedFlags = true;
            }
        } else if ( _revisions[int(remote)] ) {
            // Removing a remote revision.
            // First replace its Dict with null, then remove trailing nulls from the revision array.
            mutateRevisions();
            _mutatedRevisions[int(remote)] = Value::null();
            auto n                         = _mutatedRevisions.count();
            while ( n > 0 && !_mutatedRevisions.get(n - 1).asDict() ) --n;
            _mutatedRevisions.resize(n);
            _changed = changedFlags = true;
        }

        if ( changedFlags ) updateDocFlags();
    }

#pragma mark - CURRENT REVISION:

    slice VectorRecord::currentRevisionData() const {
        requireBody();
        return _bodyDoc.data();
    }

    void VectorRecord::setCurrentRevision(const Revision& rev) {
        setRevID(rev.revID);
        setProperties(rev.properties);
        setFlags(rev.flags);
    }

    Dict VectorRecord::originalProperties() const {
        requireBody();
        return _bodyDoc.asDict();
    }

    MutableDict VectorRecord::mutableProperties() {
        requireBody();
        MutableDict mutProperties = _current.properties.asMutable();
        if ( !mutProperties ) {
            // Make a mutable copy of the current properties:
            mutProperties = _current.properties.mutableCopy();
            if ( !mutProperties ) mutProperties = MutableDict::newDict();
            _current.properties = mutProperties;
            _currentProperties  = mutProperties;
        }
        return mutProperties;
    }

    void VectorRecord::setProperties(Dict newProperties) {
        requireBody();
        if ( newProperties != _current.properties ) {
            _currentProperties  = newProperties;
            _current.properties = newProperties;
            _changed            = true;
        }
    }

    void VectorRecord::setRevID(revid newRevID) {
        requireBody();
        if ( !newRevID ) error::_throw(error::InvalidParameter);
        if ( newRevID != _current.revID ) {
            _revID         = alloc_slice(newRevID);
            _current.revID = revid(_revID);
            _changed       = true;
        }
    }

    void VectorRecord::setFlags(DocumentFlags newFlags) {
        Assert((uint8_t(newFlags) & ~0x5) == 0);  // only kDeleted and kHasAttachments are legal
        requireBody();
        if ( newFlags != _current.flags ) {
            _current.flags = newFlags;
            _changed       = true;
            updateDocFlags();
        }
    }

    revid VectorRecord::lastLegacyRevID() const {
        requireRemotes();
        return revid(_revisions[0].asDict()[kLegacyRevIDKey].asData());
    }

#pragma mark - CHANGE HANDLING:

    void VectorRecord::updateDocFlags() {
        // Take the local revision's flags, and add the Conflicted and Attachments flags
        // if any remote rev has them.
        auto newDocFlags = DocumentFlags::kNone;
        if ( _docFlags & DocumentFlags::kSynced ) newDocFlags |= DocumentFlags::kSynced;

        newDocFlags = newDocFlags | _current.flags;
        for ( Array::iterator i(_revisions); i; ++i ) {
            Dict revDict = i.value().asDict();
            if ( revDict ) {
                auto flags = DocumentFlags(revDict[kRevFlagsKey].asInt());
                if ( flags & DocumentFlags::kConflicted ) newDocFlags |= DocumentFlags::kConflicted;
                if ( flags & DocumentFlags::kHasAttachments ) newDocFlags |= DocumentFlags::kHasAttachments;
            }
        }
        _docFlags = newDocFlags;
    }

    bool VectorRecord::changed() const { return _changed || propertiesChanged(); }

    bool VectorRecord::propertiesChanged() const {
        for ( DeepIterator i(_current.properties); i; ++i ) {
            if ( Value val = i.value(); val.isMutable() ) {
                if ( auto dict = val.asDict(); dict ) {
                    if ( dict.asMutable().isChanged() ) return true;
                } else if ( auto array = val.asArray(); array ) {
                    if ( array.asMutable().isChanged() ) return true;
                }
            } else {
                i.skipChildren();
            }
        }
        return false;
    }

    void VectorRecord::clearPropertiesChanged() const {
        for ( DeepIterator i(_current.properties); i; ++i ) {
            if ( Value val = i.value(); val.isMutable() ) {
                if ( auto dict = val.asDict(); dict ) FLMutableDict_SetChanged(dict.asMutable(), false);
                else if ( auto array = val.asArray(); array )
                    FLMutableArray_SetChanged(array.asMutable(), false);
            } else {
                i.skipChildren();
            }
        }
    }

#pragma mark - SAVING:

    VectorRecord::SaveResult VectorRecord::save(ExclusiveTransaction& transaction, HybridClock& versionClock) {
        requireRemotes();
        auto [props, revID, flags] = currentRevision();
        props                      = nullptr;  // unused
        bool newRevision           = !revID || propertiesChanged();
        if ( !newRevision && !_changed ) return kNoSave;

        // If the revID hasn't been changed but the local properties have, generate a new revID:
        alloc_slice generatedRev;
        if ( newRevision && _revID == _savedRevID ) {
            generatedRev = generateVersionVector(revID, versionClock);
            revID        = revid(generatedRev);
            setRevID(revID);
            LogTo(DBLog, "Doc %.*s generated revID '%s'", FMTSLICE(_docID), revID.str().c_str());
        }

        Assert(revID.isVersion());
        if ( _savedRevID && !revid(_savedRevID).isVersion() ) {
            LogToAt(DBLog, Verbose, "Doc %.*s saving legacy revID '%s'; new revID '%s'", FMTSLICE(_docID),
                    revid(_savedRevID).str().c_str(), revID.str().c_str());
            mutableRevisionDict(RemoteID::Local)[kLegacyRevIDKey].setData(_savedRevID);
        }

        alloc_slice body, extra;
        tie(body, extra) = encodeBodyAndExtra();

        bool updateSequence = (_sequence == 0_seq || _revID != _savedRevID);
        Assert(revID);
        RecordUpdate rec(_docID, body, _docFlags);
        rec.version     = revID;
        rec.extra       = extra;
        rec.sequence    = _sequence;
        rec.subsequence = _subsequence;
        auto seq        = _store.set(rec, KeyStore::flagUpdateSequence(updateSequence), transaction);
        if ( seq == 0_seq ) return kConflict;

        _sequence    = seq;
        _subsequence = updateSequence ? 0 : _subsequence + 1;
        _savedRevID  = _revID;
        _changed     = false;

        // Update Fleece Doc to newly saved data:
        MutableDict mutableProperties = _current.properties.asMutable();
        readRecordBody(body);
        readRecordExtra(extra);
        if ( mutableProperties ) {
            // The client might still have references to mutable objects under _properties,
            // so keep that mutable Dict as the current _properties:
            _current.properties = mutableProperties;
            _currentProperties  = mutableProperties;
            clearPropertiesChanged();
        }

        return updateSequence ? kNewSequence : kNoNewSequence;
    }

    pair<alloc_slice, alloc_slice> VectorRecord::encodeBodyAndExtra() {
        return _encoder ? encodeBodyAndExtra(_encoder) : encodeBodyAndExtra(Encoder(sharedKeys()));
    }

    pair<alloc_slice, alloc_slice> VectorRecord::encodeBodyAndExtra(FLEncoder flEnc) {
        SharedEncoder enc(flEnc);
        alloc_slice   body, extra;
        unsigned      nRevs = _revisions.count();
        if ( nRevs == 0 ) {
            // Only a current rev, nothing else, so only generate a body:
            if ( !_current.properties.empty() ) {
                enc.writeValue(_current.properties);
                body = enc.finish();
            }
        } else {
            enc.beginArray();
            DeDuplicateEncoder ddenc(enc);
            // Write current rev. The body dict will be written first; then we snip that as the
            // record's `body` property, and the encoder will write the rest of the `extra` with
            // a back-pointer into the `body`.
            enc.beginDict();
            enc.writeKey(kRevPropertiesKey);
            ddenc.writeValue(_current.properties, 1);
            body = alloc_slice(FLEncoder_Snip(enc));
            if ( revid legacy = lastLegacyRevID() ) {
                enc.writeKey(kLegacyRevIDKey);
                enc.writeData(legacy);
            }
            enc.endDict();

            // Write other revs:
            for ( unsigned i = 1; i < nRevs; i++ ) {
                Value rev = _revisions.get(i);
                ddenc.writeValue(rev, 2);
            }
            enc.endArray();
            extra = enc.finish();
        }

        return {body, extra};
    }

    alloc_slice VectorRecord::generateRevID(Dict body, revid parentRevID, DocumentFlags flags) {
        // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and JSON:
        alloc_slice json = FLValue_ToJSONX(body, false, true);
        parentRevID.setSize(min(parentRevID.size, size_t(255)));
        auto     revLen     = (uint8_t)parentRevID.size;
        uint8_t  delByte    = (flags & DocumentFlags::kDeleted) != 0;
        SHA1     digest     = (SHA1Builder() << revLen << parentRevID << delByte << json).finish();
        unsigned generation = parentRevID ? parentRevID.generation() + 1 : 1;
        return alloc_slice(revidBuffer(generation, slice(digest)).getRevID());
    }

    alloc_slice VectorRecord::generateVersionVector(revid parentRevID, HybridClock& versionClock) {
        VersionVector vec;
        if ( parentRevID ) vec = parentRevID.asVersionVector();
        vec.addNewVersion(versionClock);
        return vec.asBinary();
    }

#pragma mark - TESTING:

    void VectorRecord::dump(ostream& out) const {
        out << "\"" << (string)docID() << "\" #" << uint64_t(sequence()) << " ";
        uint32_t nRevs = std::max(_revisions.count(), uint32_t(1));
        for ( uint32_t i = 0; i < nRevs; ++i ) {
            optional<Revision> rev = remoteRevision(RemoteID(i));
            if ( rev ) {
                if ( i > 0 ) out << "; R" << i << '@';
                if ( rev->revID ) out << rev->revID.str();
                else
                    out << "--";
                if ( rev->flags != DocumentFlags::kNone ) {
                    out << "(";
                    if ( rev->isDeleted() ) out << "D";
                    if ( rev->isConflicted() ) out << "C";
                    if ( rev->hasAttachments() ) out << "A";
                    out << ')';
                }
            }
        }
        if ( _whichContent < kEntireBody ) out << "[other revs not loaded]";
    }

    string VectorRecord::dump() const {
        stringstream out;
        dump(out);
        return out.str();
    }

}  // namespace litecore

#pragma mark - INTERNALS:


// This stuff is kept below because the rest because it uses the Fleece "impl" API,
// and having both APIs in scope gets confusing and leads to weird compiler errors.

#include "Doc.hh"  // fleece::impl::Doc

namespace litecore {

    // Subclass of Doc that points back to the VectorRecord instance. That way when we use
    // Scope::containing to look up where a Fleece Value* is, we can track it back to the
    // VectorRecord that owns the Doc.
    class VectorRecord::LinkedFleeceDoc : public fleece::impl::Doc {
      public:
        LinkedFleeceDoc(const alloc_slice& fleeceData, FLTrust trust, fleece::impl::SharedKeys* sk,
                        VectorRecord* document_)
            : fleece::impl::Doc(fleeceData, Doc::Trust(trust), sk), document(document_) {}

        VectorRecord* const document;
    };

    FLSharedKeys VectorRecord::sharedKeys() const { return (FLSharedKeys)_store.dataFile().documentKeys(); }

    Doc VectorRecord::newLinkedFleeceDoc(const alloc_slice& body, FLTrust trust) {
        auto sk = _store.dataFile().documentKeys();
        return (FLDoc) new LinkedFleeceDoc(body, trust, sk, this);
    }

    VectorRecord* VectorRecord::containing(Value value) {
        if ( value.isMutable() ) {
            // Scope doesn't know about mutable Values (they're in the heap), but the mutable
            // Value may be a mutable copy of a Value with scope...
            if ( value.asDict() ) value = value.asDict().asMutable().source();
            else
                value = value.asArray().asMutable().source();
            if ( !value ) return nullptr;
        }

        const impl::Scope* scope = impl::Scope::containing((const impl::Value*)(FLValue)value);
        if ( !scope ) return nullptr;
        auto versScope = dynamic_cast<const LinkedFleeceDoc*>(scope);
        if ( !versScope ) return nullptr;
        return versScope->document;
    }

    string VectorRecord::dumpStorage() const {
        stringstream out;
        if ( _bodyDoc ) {
            slice data = _bodyDoc.allocedData();
            out << "---BODY: " << data.size << " bytes at " << (const void*)data.buf << ":\n";
            fleece::impl::Value::dump(data, out);
        }
        if ( _extraDoc ) {
            slice data = _extraDoc.allocedData();
            out << "---EXTRA: " << data.size << " bytes at " << (const void*)data.buf << ":\n";
            fleece::impl::Value::dump(data, out);
        }
        return out.str();
    }

    /*static*/ void VectorRecord::forAllRevIDs(const RecordUpdate& rec, const ForAllRevIDsCallback& callback) {
        if ( revid(rec.version).isVersion() ) {
            callback(RemoteID::Local, revid(rec.version), rec.body.size > 0);
            if ( rec.extra.size > 0 ) {
                fleece::impl::Scope scope(rec.extra, nullptr, rec.body);
                Array               remotes = ValueFromData(rec.extra, kFLTrusted).asArray();
                int                 n       = 0;
                for ( Array::iterator i(remotes); i; ++i, ++n ) {
                    if ( n > 0 ) {
                        Dict remote = i.value().asDict();
                        if ( slice revID = remote[kRevIDKey].asData(); revID )
                            callback(RemoteID(n), revid(revID), remote[kRevPropertiesKey] != nullptr);
                    }
                }
            }
        } else {
            // Legacy RevTree record:
            RevTree    revTree(rec.body, rec.extra, rec.sequence);
            const Rev* curRev = revTree.currentRevision();
            // First the local version:
            callback(RemoteID::Local, curRev->revID, curRev->isBodyAvailable());
            // Then the remotes:
            for ( auto [id, rev] : revTree.remoteRevisions() ) {
                if ( rev != curRev ) { callback(RemoteID(id), rev->revID, rev->isBodyAvailable()); }
            }
        }
    }

}  // namespace litecore
