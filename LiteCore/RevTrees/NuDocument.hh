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


    /// Persistent local identifier of a remote database that replicates with this one.
    /// This is used as a tag, to let NuDocument remember the last-known revision of a document in that
    /// database. This allows the replicator to generate and apply deltas when replicating.
    /// It's also used when the remote replicator runs in no-conflict mode and requires that we identify the
    /// parent revision when pushing an update.
    ///
    /// RemoteIDs must be positive. They are assigned by the C4Database, which stores a list of remote
    /// database URLs. NuDocument assumes that RemoteIDs are small numbers and uses them as array indexes.
    enum class RemoteID: int {
        Local = 0       /// Refers to the local revision, not a remote.
    };


    /// Rewritten document class for 3.0.
    /// Instead of a revision tree, it stores the _current_ local revision, and may store the current
    /// revision for each database this one replicates with, as indexed by `RemoteID`.
    /// It attempts to optimize storage when these revisions are either identical or share common values.
    class NuDocument {
    public:
        using Dict = fleece::Dict;
        using MutableDict = fleece::MutableDict;

        /// Reads a document given a Record. If the document doesn't exist, the resulting NuDocument will be
        /// empty, with an empty `properties` Dict and a null revision ID.
        NuDocument(KeyStore&, const Record&);

        /// Reads a document given the docID.
        NuDocument(KeyStore& store, slice docID);

        ~NuDocument();

        /// Given a Fleece Value, finds the NuDocument it belongs to.
        static NuDocument* containing(fleece::Value);

        /// Sets a custom Fleece Encoder to use when saving.
        void setEncoder(FLEncoder enc)                      {_encoder = enc;}

        /// Returns true if the document exists in the database.
        bool exists() const noexcept FLPURE                 {return _sequence > 0;}

        
        //---- Metadata:

        /// The document's sequence number, or 0 if it doesn't exist in the database yet.
        sequence_t sequence() const noexcept FLPURE         {return _sequence;}

        /// The document ID.
        const alloc_slice& docID() const noexcept FLPURE    {return _docID;}

        /// The current revision's properties. Never null, but is empty if this is a new document.
        Dict properties() const noexcept FLPURE             {return _properties;}

        /// The current revision ID, initially a null slice in a new document.
        revid revID() const FLPURE                          {return currentRevision().revID;}

        /// The current document flags (kDeleted, kHasAttachments, kConflicted.)
        DocumentFlags flags() const FLPURE                  {return currentRevision().flags;}

        /// Returns the current properties, revID and flags in a single struct.
        /// \warning  The `properties` and `revID` fields point to the _current_ values. Calling
        ///          \ref setProperties. \ref setRevID or \ref save will invalidate those pointers.
        Revision currentRevision() const FLPURE           {return *remoteRevision(RemoteID::Local);}


        //---- Modifying the document:

        /// The properties as a mutable Dict, that can be modified in place.
        /// \warning  The first time \ref mutableProperties is called, the value returned by \ref properties
        ///          will change: it will now return a reference to the same mutable Dict.
        ///          (The prior Dict does remain valid until \ref save is called, though.)
        ///          If you don't want this behavior, then either always call \ref mutableProperties, or
        ///          call it once before the first call to \ref properties.
        MutableDict mutableProperties();

        /// Replaces the current properties with a new Dict.
        /// (This is a no-op if the Dict is pointer-equal to the \ref properties or \ref mutableProperties.)
        /// \note The preferred way to change the properties of an existing document is to mutate the Dict
        ///       returned by \ref mutableProperties. This allows the document to detect which properties
        ///       are unchanged, which generally reduces storage space and optimizes delta operations.
        void setProperties(Dict);

        /// Assigns a custom revision ID for the current in-memory changes. When saving, this revID will be
        /// assigned to the new revision. If you don't call this, a revID will be auto-generated by the
        /// \ref save method. (This method is normally only called by the replicator.)
        void setRevID(revid);

        /// Sets the flags of the in-memory revision that will be saved by the \ref save method.
        /// This should generally be called before saving, otherwise the flags of the previous revision will
        /// be reused, which may not be appropriate.
        void setFlags(DocumentFlags);

        /// Sets the properties, revID and flags at once.
        /// \note  See the note on the \ref setProperties method about performance implications of storing
        ///       a different properties Dict.
        void setCurrentRevision(const Revision &rev)      {setRemoteRevision(RemoteID::Local, rev);}

        /// Returns true if in-memory state of this object has changed since it was created or last saved.
        bool changed() const FLPURE;

        /// Possible results of the \ref save method.
        enum SaveResult {
            kConflict,      ///< The document was **not saved** because a newer revision already exists.
            kNoSave,        ///< There were no changes to save.
            kNoNewSequence, ///< The document was saved, but the local revision was unchanged so no new sequence number was assigned.
            kNewSequence    ///< The document was saved and a new sequence number assigned.
        };

        /// Saves changes, if any, back to the KeyStore.
        /// \note  Most errors are thrown as exceptions, but a conflict is returned as `kConflict`.
        SaveResult save(Transaction &t);


        //---- Revisions of different remotes:

        /// Returns the current revision stored for the given \ref RemoteID, if any.
        /// \warning  The `properties` and `revID` fields point to the _current_ values. Calling
        ///          \ref setRemoteRevision or \ref save will invalidate those pointers.
        std::optional<Revision> remoteRevision(RemoteID) const FLPURE;

        /// Stores a revision for the given \ref RemoteID, or removes it if the revision is `nullopt`.
        void setRemoteRevision(RemoteID, const std::optional<Revision>&);


        //---- For testing:

        /// Generates a revision ID given document properties, parent revision ID, and flags.
        static revidBuffer generateRevID(Dict, revid parentRevID, DocumentFlags);

        std::string dump() const;           ///< Returns an ASCII dump of the object's state.
        void dump(std::ostream&) const;     ///< Writes an ASCII dump of the object's state.
        std::string dumpStorage() const;    ///< Returns the JSON form of the internal storage.
        fleece::Array revisionStorage() const   {return _revisions;} ///< The internal storage

    private:
        class LinkedFleeceDoc;

        bool initFleeceDoc(const alloc_slice &body);
        fleece::Array savedRevisions() const;
        void mutateRevisions();
        MutableDict mutableRevisionDict(RemoteID remoteID);
        Dict originalProperties() const;
        alloc_slice encodeBody(FLEncoder);
        bool propertiesChanged() const;
        void clearPropertiesChanged();

        KeyStore&                    _store;                // The database KeyStore
        fleece::SharedKeys           _sharedKeys;           // Fleece shared keys (global to DB)
        FLEncoder                    _encoder {nullptr};    // Database shared Fleece Encoder
        alloc_slice                  _docID;                // The docID
        sequence_t                   _sequence;             // The sequence
        Retained<LinkedFleeceDoc>    _fleeceDoc;            // If saved, a Doc of the Fleece body
        fleece::Array                _revisions;            // Top-level parsed body; stores revs
        mutable fleece::MutableArray _mutatedRevisions;     // Mutable version of `_revisions`
        Dict                         _properties;           // Current revision properties
        bool                         _changed {false};      // Set to true on explicit change
        bool                         _revIDChanged {false}; // Has setRevID() been called?
        // (Note: _changed doesn't reflect mutations to _properties; changed() checks for those.)
    };
}
