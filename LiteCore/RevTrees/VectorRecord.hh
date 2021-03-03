//
// VectorRecord.hh
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
#include <optional>

namespace litecore {
    class KeyStore;
    class Transaction;
    class Version;
    class VersionVector;


    /// Metadata and properties of a document revision.
    struct Revision {
        /// The root of the document's properties.
        /// \warning  Mutating the owning \ref VectorRecord will invalidate this pointer!
        fleece::Dict  properties;

        /// The encoded version/revision ID. Typically this stores a VersionVector.
        revid revID;

        /// The revision's flags:
        /// - kDeleted: This is a tombstone
        /// - kConflicted: This is a conflict with the current local revision
        /// - kHasAttachments: Properties include references to blobs.
        DocumentFlags flags {};

        /// Returns the current (first) version of the version vector encoded in the `revID`.
        Version version() const;

        /// Decodes the entire version vector encoded in the `revID`. (This allocates heap space.)
        VersionVector versionVector() const;

        bool isDeleted() const FLPURE      {return flags & DocumentFlags::kDeleted;}
        bool isConflicted() const FLPURE   {return flags & DocumentFlags::kConflicted;}
        bool hasAttachments() const FLPURE {return flags & DocumentFlags::kHasAttachments;}
    };


    /// Type of revision versioning to use: rev-trees or version vectors.
    enum class Versioning : uint8_t {
        RevTrees,
        Vectors
    };


    /// Persistent local identifier of a remote database that replicates with this one.
    /// This is used as a tag, to let VectorRecord remember the last-known revision of a document in that
    /// database. This allows the replicator to generate and apply deltas when replicating.
    /// It's also used when the remote replicator runs in no-conflict mode and requires that we identify the
    /// parent revision when pushing an update.
    ///
    /// RemoteIDs must be positive. They are assigned by the C4Database, which stores a list of remote
    /// database URLs.
    /// \note VectorRecord's current implementation assumes that RemoteIDs are small consecutive numbers
    ///      starting at 0, and so uses them as array indexes.
    enum class RemoteID: int {
        Local = 0       /// Refers to the local revision, not a remote.
    };


    /// Rewritten document class for 3.0.
    /// Instead of a revision tree, it stores the _current_ local revision, and may store the current
    /// revision for each database this one replicates with, as indexed by its `RemoteID`.
    ///
    /// It attempts to optimize storage when these revisions are either identical or share common property
    /// values.
    class VectorRecord {
    public:
        using Dict = fleece::Dict;
        using MutableDict = fleece::MutableDict;

        /// Reads a document given a Record. If the document doesn't exist, the resulting VectorRecord will be
        /// empty, with an empty `properties` Dict and a null revision ID.
        VectorRecord(KeyStore&, Versioning, const Record&);

        /// Reads a document given the docID.
        VectorRecord(KeyStore& store, Versioning, slice docID, ContentOption =kEntireBody);

        ~VectorRecord();

        /// You can store a pointer to whatever you want here.
        void* owner = nullptr;

        /// Given a Fleece Value, finds the VectorRecord it belongs to.
        static VectorRecord* containing(fleece::Value);

        /// Sets a custom Fleece Encoder to use when saving.
        void setEncoder(FLEncoder enc)                      {_encoder = enc;}

        /// Returns true if the document exists in the database.
        bool exists() const noexcept FLPURE                 {return _sequence > 0;}

        /// Returns what content has been loaded: metadata, current revision, or all revisions.
        ContentOption contentAvailable() const noexcept FLPURE {return _whichContent;}

        /// If the requested content isn't in memory, loads it.
        /// Returns true if the content is now available, false if it couldn't be loaded.
        bool loadData(ContentOption which =kEntireBody);
        
        //---- Metadata:

        /// The document's sequence number, or 0 if it doesn't exist in the database yet.
        sequence_t sequence() const noexcept FLPURE         {return _sequence;}

        /// The document ID.
        const alloc_slice& docID() const noexcept FLPURE    {return _docID;}

        /// The current revision's properties. Never null, but is empty if this is a new document.
        Dict properties() const noexcept FLPURE             {return _current.properties;}

        /// The current revision ID, initially a null slice in a new document.
        revid revID() const FLPURE                          {return _current.revID;}

        /// The current document flags (kDeleted, kHasAttachments, kConflicted.)
        DocumentFlags flags() const FLPURE                  {return _docFlags;}

        /// Returns the current properties, revID and flags in a single struct.
        /// \warning  The `properties` and `revID` fields point to the _current_ values. Calling
        ///          \ref setProperties. \ref setRevID or \ref save will invalidate those pointers.
        Revision currentRevision() const FLPURE             {return _current;}

        /// The current revision's encoded Fleece data.
        slice currentRevisionData() const;

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
        void setCurrentRevision(const Revision &rev);

        /// Returns true if in-memory state of this object has changed since it was created or last saved.
        bool changed() const FLPURE;

        //---- Saving:

        /// Possible results of the \ref save method.
        enum SaveResult {
            kConflict,      ///< The document was **not saved** because a newer revision already exists.
            kNoSave,        ///< There were no changes to save.
            kNoNewSequence, ///< Saved, but the local rev was unchanged so no new sequence was assigned.
            kNewSequence    ///< The document was saved and a new sequence number assigned.
        };

        /// Saves changes, if any, back to the KeyStore.
        /// \note  Most errors are thrown as exceptions, but a conflict is returned as `kConflict`.
        SaveResult save(Transaction &t);

        /// Returns the `body` and `extra` Record values representing the current in-memory state.
        /// This is used by the \ref save method and the database upgrader. Shouldn't be needed elsewhere.
        pair<alloc_slice,alloc_slice> encodeBodyAndExtra();

        //---- Revisions of different remotes:

        /// Returns the current revision stored for the given \ref RemoteID, if any.
        /// \warning  The `properties` and `revID` fields point to the _current_ values. Calling
        ///          \ref setRemoteRevision or \ref save will invalidate those pointers.
        std::optional<Revision> remoteRevision(RemoteID) const FLPURE;

        /// Same as \ref remoteRevision, but loads the document's remote revisions if not in memory yet.
        std::optional<Revision> loadRemoteRevision(RemoteID);

        /// Stores a revision for the given \ref RemoteID, or removes it if the revision is `nullopt`.
        void setRemoteRevision(RemoteID, const std::optional<Revision>&);

        /// Returns the next RemoteID for which a revision is stored.
        RemoteID nextRemoteID(RemoteID) const;

        /// Same as \ref nextRemoteID, but loads the document's remote revisions if not in memory yet.
        RemoteID loadNextRemoteID(RemoteID);

        using ForAllRevIDsCallback = function_ref<void(RemoteID,revid,bool hasBody)>;

        /// Given only a record, find all the revision IDs and pass them to the callback.
        static void forAllRevIDs(const RecordUpdate&, const ForAllRevIDsCallback&);

        //---- For testing:

        /// Generates a rev-tree revision ID given document properties, parent revision ID, and flags.
        static alloc_slice generateRevID(Dict, revid parentRevID, DocumentFlags);
        /// Generates a version-vector revision ID given parent vector.
        static alloc_slice generateVersionVector(revid parentVersionVector);

        std::string dump() const;           ///< Returns an ASCII dump of the object's state.
        void dump(std::ostream&) const;     ///< Writes an ASCII dump of the object's state.
        std::string dumpStorage() const;    ///< Returns the JSON form of the internal storage.
        fleece::Array revisionStorage() const   {return _revisions;} ///< The internal storage

    private:
        class LinkedFleeceDoc;

        FLSharedKeys sharedKeys() const;
        fleece::Doc newLinkedFleeceDoc(const alloc_slice &body);
        void readRecordBody(const alloc_slice &body);
        void readRecordExtra(const alloc_slice &extra);
        void requireBody() const;
        void requireRemotes() const;
        void mustLoadRemotes();
        void mutateRevisions();
        MutableDict mutableRevisionDict(RemoteID remoteID);
        Dict originalProperties() const;
        pair<alloc_slice,alloc_slice> encodeBodyAndExtra(FLEncoder NONNULL);
        alloc_slice encodeExtra(FLEncoder NONNULL);
        bool propertiesChanged() const;
        void clearPropertiesChanged();
        void updateDocFlags();

        KeyStore&                    _store;                // The database KeyStore
        FLEncoder                    _encoder {nullptr};    // Database shared Fleece Encoder
        alloc_slice                  _docID;                // The docID
        sequence_t                   _sequence;             // The Record's sequence
        sequence_t                   _subsequence;          // The Record's subsequence
        DocumentFlags                _docFlags;             // Document-level flags
        alloc_slice                  _revID;                // Current revision ID backing store
        Revision                     _current;              // Current revision
        fleece::RetainedValue        _currentProperties;    // Retains local properties
        fleece::Doc                  _bodyDoc;              // If saved, a Doc of the Fleece body
        fleece::Doc                  _extraDoc;             // Fleece Doc holding record `extra`
        fleece::Array                _revisions;            // Top-level parsed body; stores revs
        mutable fleece::MutableArray _mutatedRevisions;     // Mutable version of `_revisions`
        Versioning                   _versioning;           // RevIDs or VersionVectors?
        bool                         _changed {false};      // Set to true on explicit change
        bool                         _revIDChanged {false}; // Has setRevID() been called?
        ContentOption                _whichContent;         // Which parts of record are available
        // (Note: _changed doesn't reflect mutations to _properties; changed() checks for those.)
    };
}
