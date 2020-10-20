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
#include "Error.hh"
#include "Defer.hh"
#include <ostream>
#include <sstream>

namespace litecore {
    using namespace std;
    using namespace fleece;

    static constexpr slice kMetaBody  = "body";
    static constexpr slice kMetaRevID = "revID";
    static constexpr slice kMetaFlags = "flags";


    NuDocument::NuDocument(KeyStore& store, slice docID)
    :_store(store), _rec(docID)
    {
        _store.read(_rec);
        decode();
    }


    NuDocument::NuDocument(KeyStore& store, const Record& rec)
    :_store(store), _rec(rec)
    {
        decode();
    }


    NuDocument::~NuDocument() { }


    void NuDocument::decode() {
        if (initFleeceDoc()) {
            if (!_revisions)
                error::_throw(error::CorruptRevisionData);
            _properties = _revisions[kLocal].asDict();
            if (!_properties)
                error::_throw(error::CorruptRevisionData);
        } else {
            // "Untitled" empty state:
            (void)mutableProperties();
        }
    }


#pragma mark - REVISIONS:


    optional<Revision> NuDocument::remoteRevision(RemoteID remote) const {
        if (remote == kLocal) {
            return currentRevision();
        } else if (Dict revDict = _revisions[remote].asDict(); revDict) {
            // Non-local revisions have a top-level dict with the revID, flags, properties.
            Dict properties = revDict[kMetaBody].asDict();
            revid revID(revDict[kMetaRevID].asData());
            auto flags = DocumentFlags(revDict[kMetaFlags].asInt());
            if (!properties || !revID)
                error::_throw(error::CorruptRevisionData);
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


    void NuDocument::setRemoteRevision(RemoteID remoteID, const optional<Revision> &newRev) {
        Assert(remoteID != kLocal);
        if (!newRev && !_revisions[remoteID])
            return;
        if (newRev) {
            // Creating/updating a remote revision:
            MutableDict revDict = mutableRevisionDict(remoteID);
            revDict[kMetaBody] = newRev->properties;
            revDict[kMetaRevID].setData(newRev->revID);
            revDict[kMetaFlags] = int(newRev->flags);

        } else {
            // Removing a remote revision:
            mutateRevisions();
            _mutatedRevisions[remoteID] = Value::null();
            auto n = _mutatedRevisions.count();
            while (n > 0 && !_mutatedRevisions[n-1].asDict())
                --n;
            _mutatedRevisions.resize(n);
        }

        _changed = true;
    }


#pragma mark - DOCUMENT ROOT/PROPERTIES:


    MutableDict NuDocument::mutableProperties() {
       if (!_mutatedProperties)
           _properties = _mutatedProperties = mutableRevisionDict(kLocal);
        return _mutatedProperties;
    }


    void NuDocument::setProperties(Dict properties) {
        if (properties == _properties)
            return;
        if (properties) {
            auto mutProperties = properties.asMutable();
            if (!mutProperties)
                mutProperties = properties.mutableCopy();
            _properties = _mutatedProperties = mutProperties;
        } else {
            // Clearing properties (presumably deleting):
            _properties = nullptr;
            _mutatedProperties = nullptr;
        }
        _changed = true;
    }


    bool NuDocument::changed() const {
        return _changed || (_mutatedProperties && _mutatedProperties.isChanged());
    }


#pragma mark - SAVING:


    bool NuDocument::shouldIncrementallyEncode() const {
        // Check if the current properties dict is a mutated version of the stored one:
        auto source = _mutatedProperties.source();
        return source && NuDocument::containing(source) == this;
    }


    alloc_slice NuDocument::encodeBody(FLEncoder flEnc) {
        SharedEncoder enc(flEnc);
        bool incremental = shouldIncrementallyEncode();
        if (incremental)
            enc.amend(_rec.body());  // we will be writing a patch to be appended to the body

        if (_revisions.count() == 1) {
            enc.writeValue(_revisions);
        } else {
            map<FLValue,ssize_t> writtenRevs;

            // Subroutine that encodes a revision body, using a pointer if that exact Dict has
            // already been encoded. This saves space when multiple revisions are identical.
            auto writeBody = [&](Value body) {
                if (auto e = writtenRevs.find(body); e != writtenRevs.end() && e->second != 0) {
                    FLEncoder_WriteValueAgain(enc, e->second);
                } else {
                    enc.writeValue(body);
                    writtenRevs[body] = FLEncoder_LastValueWritten(enc);
                }
            };

            enc.beginArray();
            RemoteID remote = 0;
            for (Array::iterator iRev(_revisions); iRev; ++iRev, ++remote) {
                if (remote == 0) {
                    // _revisions[0] is just the local body
                    writeBody(iRev.value());
                } else {
                    // other items are metadata dicts:
                    Dict meta = iRev.value().asDict();
                    enc.beginDict();
                    for (Dict::iterator i(meta); i; ++i) {
                        slice key = i.keyString();
                        Value value = i.value();
                        enc.writeKey(key);
                        if (key == kMetaBody)
                            writeBody(value);
                        else
                            enc.writeValue(value);
                    }
                    enc.endDict();
                }
            }
            enc.endArray();
        }

        alloc_slice encoded = enc.finish();
        if (incremental) {
            auto body = _rec.body();
            body.append(encoded);
            encoded = body;
        }
        return encoded;
    }


    alloc_slice NuDocument::encodeBody() {
        if (!_properties)
            return nullslice;
        else if (_encoder)
            return encodeBody(_encoder);
        else
            return encodeBody(Encoder(_sharedKeys));
    }


    NuDocument::SaveResult NuDocument::save(Transaction& transaction,
                                            revid newRevID,
                                            DocumentFlags newFlags)
    {
        bool revIDChanged = (newRevID != revID());
        if (!revIDChanged && newFlags == flags() && !changed())
            return kNoSave;
        if (revIDChanged)
            _rec.setVersion(newRevID);
        _rec.setFlags(newFlags);
        _rec.setBody(encodeBody());
        _changed = true;
        SaveResult result = (revIDChanged || sequence() == 0) ? kNewSequence : kNoNewSequence;

        if (result == kNoSave)
            return result;

        sequence_t seq = sequence();
        seq = _store.write(_rec, transaction, seq, (result == kNewSequence));
        if (seq == 0)
            return kConflict;

        // Go back to unchanged state, by reloading from the Record:
        _changed = _revIDChanged = false;
        _fleeceDoc = nullptr;
        _revisions = nullptr;
        _mutatedRevisions = nullptr;
        _properties = nullptr;
        _mutatedProperties = nullptr;
        decode();
        return result;
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


    bool NuDocument::initFleeceDoc() {
        auto sk = _store.dataFile().documentKeys();
        _sharedKeys = FLSharedKeys(sk);
        if (auto body = _rec.body(); body) {
            _fleeceDoc = new LinkedFleeceDoc(body, sk, this);
            _revisions = (FLArray)_fleeceDoc->asArray();
            return true;
        } else {
            return false;
        }
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
        return fleece::impl::Value::dump(_rec.body());
    }

}
