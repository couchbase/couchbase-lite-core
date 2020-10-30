//
// NuDocument.hh
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

#pragma once
#include "Record.hh"
#include "RevID.hh"
#include "fleece/Fleece.hh"
#include "fleece/Mutable.hh"
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

namespace litecore {
    class KeyStore;
    class Transaction;


    /// Metadata and properties of a document revision.
    struct Revision {
        fleece::Dict  properties;
        revid         revID;
        DocumentFlags flags;

        bool isDeleted() const FLPURE      {return (flags & DocumentFlags::kDeleted) != 0;}
        bool isConflicted() const FLPURE   {return (flags & DocumentFlags::kConflicted) != 0;}
        bool hasAttachments() const FLPURE {return (flags & DocumentFlags::kHasAttachments) != 0;}
    };


    /// Rewritten document class for 3.0
    class NuDocument {
    public:
        using RemoteID = unsigned;
        static constexpr RemoteID kLocal = 0;

        using Dict = fleece::Dict;
        using MutableDict = fleece::MutableDict;

        NuDocument(KeyStore&, slice docID);
        NuDocument(KeyStore&, const Record&);
        ~NuDocument();

        /// Given a Fleece Value, finds the NuDocument it belongs to.
        static NuDocument* containing(fleece::Value);

        bool exists() const noexcept FLPURE                 {return _fleeceDoc != nullptr;}

        //---- Metadata:

        sequence_t sequence() const noexcept FLPURE         {return _sequence;}

        const alloc_slice& docID() const noexcept FLPURE    {return _docID;}

        Dict properties() const noexcept FLPURE             {return _properties;}
        revid revID() const FLPURE;
        DocumentFlags flags() const FLPURE;

        Revision currentRevision() const FLPURE;

        //---- Modifying the document:

        /// The properties as a mutable Dict, that can be modified in place.
        MutableDict mutableProperties();

        /// Replaces the current properties with a new Dict.
        /// This is more expensive than modifying mutableProperties().
        void setProperties(Dict);
        
        void setRevID(revid);
        void setFlags(DocumentFlags);

        void setCurrentRevision(const Revision &rev);

        /// Returns true if the proprties or remote revisions have been changed. Resets to false on save.
        bool changed() const FLPURE;

        enum SaveResult {kConflict, kNoSave, kNoNewSequence, kNewSequence};

        /// Saves changes, if any, back to the KeyStore.
        /// @return  kNewSequence if a new sequence was allocated; kNoNewSequence if sequence is unchanged;
        ///         kNoSave if there were no changes to be saved.
        SaveResult save(Transaction &t);

        /// Updates the revID and flags, then saves changes.
        SaveResult save(Transaction &t, revid revID, DocumentFlags flags);

        //---- Revisions of different remotes:

        std::optional<Revision> remoteRevision(RemoteID) const FLPURE;

        void setRemoteRevision(RemoteID, const std::optional<Revision>&);


        //---- For testing:

        fleece::Array revisionStorage() const   {return _revisions;}
        std::string dump() const;
        void dump(std::ostream&) const;
        std::string dumpStorage() const;
        static revidBuffer generateRevID(Dict, revid parentRevID, DocumentFlags);

    private:
        class LinkedFleeceDoc;

        void decode(const alloc_slice &body);
        bool initFleeceDoc(const alloc_slice &body);
        alloc_slice docBody() const;
        void mutateRevisions();
        MutableDict mutableRevisionDict(RemoteID remoteID);
        alloc_slice encodeBody();
        alloc_slice encodeBody(FLEncoder);
        void generateNewRevID();

        KeyStore&                    _store;
        fleece::SharedKeys           _sharedKeys;
        FLEncoder                    _encoder {nullptr};
        alloc_slice                  _docID;
        sequence_t                   _sequence;
        Retained<LinkedFleeceDoc>    _fleeceDoc;
        fleece::Array                _revisions;
        mutable fleece::MutableArray _mutatedRevisions;
        Dict                         _properties;
        bool                         _changed {false};
        bool                         _revIDChanged {false};
    };
}
