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
        fleece::Dict  const properties;
        revid         const revID;
        DocumentFlags const flags;

        bool isDeleted() const FLPURE      {return (flags & DocumentFlags::kDeleted) != 0;}
        bool isConflicted() const FLPURE   {return (flags & DocumentFlags::kConflicted) != 0;}
        bool hasAttachments() const FLPURE {return (flags & DocumentFlags::kHasAttachments) != 0;}
    };


    /// Rewritten document class for 3.0
    class NuDocument {
    public:
        using RemoteID = unsigned;
        static constexpr RemoteID kNoRemoteID = 0;
        static constexpr RemoteID kLocal = 0;
        static constexpr RemoteID kDefaultRemoteID = 1;     // 1st (& usually only) remote server

        using Dict = fleece::Dict;
        using MutableDict = fleece::MutableDict;

        NuDocument(KeyStore&, slice docID);
        NuDocument(KeyStore&, const Record&);
        ~NuDocument();

        /// Given a Fleece Value, finds the NuDocument it belongs to.
        static NuDocument* containing(fleece::Value);

        void setSharedFLEncoder(FLEncoder enc)  {_encoder = enc;}

        bool exists() const FLPURE              {return _rec.exists();}

        sequence_t sequence() const FLPURE      {return _rec.sequence();}

        const Record& record() const FLPURE     {return _rec;}

        //---- Metadata:

        const alloc_slice& docID() const FLPURE {return _rec.key();}

        Dict properties() const                 {return _properties;}
        revid revID() const FLPURE              {return revid(_rec.version());}
        DocumentFlags flags() const FLPURE      {return _rec.flags();}

        Revision currentRevision() const FLPURE {return {properties(), revID(), flags()};}

        //---- Modifying the document:

        /// The properties as a mutable Dict, that can be modified in place.
        MutableDict mutableProperties();

        /// Replaces the current properties with a new Dict.
        /// This is more expensive than modifying mutableProperties().
        void setProperties(Dict);

        /// Returns true if the proprties or remote revisions have been changed. Resets to false on save.
        bool changed() const FLPURE;

        enum SaveResult {kConflict, kNoSave, kNoNewSequence, kNewSequence};

        /// Saves changes, if any, back to the KeyStore.
        /// @param t  The current Transaction
        /// @param revID  The new revision ID. This is allowed to be the same as the current revID;
        ///               in that case no new sequence will be allocated.
        /// @param flags  The new document flags.
        SaveResult save(Transaction &t, revid revID, DocumentFlags flags);

        /// Saves changes without creating a new revision or sequence.
        SaveResult save(Transaction &t)         {return save(t, revID(), flags());}

        //---- Revisions of different remotes:

        std::optional<Revision> remoteRevision(RemoteID) const;

        void setRemoteRevision(RemoteID, const std::optional<Revision>&);   // not for local rev


        //---- For testing:

        fleece::Array revisionStorage() const   {return _revisions;}
        std::string dump() const;
        void dump(std::ostream&) const;
        std::string dumpStorage() const;

    private:
        class LinkedFleeceDoc;

        void decode();
        bool initFleeceDoc();
        void mutateRevisions();
        MutableDict mutableRevisionDict(RemoteID remoteID);
        alloc_slice encodeBody();
        alloc_slice encodeBody(FLEncoder);
        bool shouldIncrementallyEncode() const;

        KeyStore&                    _store;
        Record                       _rec;
        Retained<LinkedFleeceDoc>    _fleeceDoc;
        fleece::SharedKeys           _sharedKeys;
        FLEncoder                    _encoder {nullptr};
        fleece::Array                _revisions;
        mutable fleece::MutableArray _mutatedRevisions;
        Dict                         _properties;
        mutable MutableDict          _mutatedProperties;
        bool                         _changed {false};
        bool                         _revIDChanged {false};
    };
}
