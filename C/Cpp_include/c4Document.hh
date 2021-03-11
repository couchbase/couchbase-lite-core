//
// c4Document.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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
#include "c4Base.hh"
#include "c4DocumentTypes.h"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN


    struct C4Document {
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        template <class T> using Retained = fleece::Retained<T>;

        C4DocumentFlags flags;      ///< Document flags
        C4HeapString docID;         ///< Document ID
        C4HeapString revID;         ///< Revision ID of current revision
        C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

        C4Revision selectedRev;     ///< Describes the currently-selected revision

        C4ExtraInfo extraInfo;      ///< For client use

        // NOTE: Instances are created with database->getDocument or database->putDocument.

        // Static utility functions:

        static constexpr size_t kGeneratedIDLength = 23;

        static char* generateID(char *outDocID,
                                size_t bufferSize) noexcept;

        static bool equalRevIDs(slice revID1,
                                slice revID2) noexcept;
        static unsigned getRevIDGeneration(slice revID) noexcept;

        static C4RevisionFlags revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept;

        /// Returns the Document instance, if any, that contains the given Fleece value.
        static C4Document* C4NULLABLE containingValue(FLValue) noexcept;

        static bool containsBlobs(FLDict) noexcept;
        static bool isOldMetaProperty(slice propertyName) noexcept;
        static bool hasOldMetaProperties(FLDict) noexcept;

        static bool isValidDocID(slice) noexcept;

        static alloc_slice encodeStrippingOldMetaProperties(FLDict properties,
                                                            FLSharedKeys);

        // Selecting revisions:

        bool selectCurrentRevision() noexcept;

        bool selectRevision(C4Slice revID, bool withBody =true);   // returns false if not found

        bool selectParentRevision() noexcept;
        bool selectNextRevision();
        bool selectNextLeafRevision(bool includeDeleted, bool withBody =true);
        bool selectCommonAncestorRevision(slice revID1, slice revID2);

        // Revision info:

        bool loadRevisionBody(); // can throw; returns false if compacted away

        bool hasRevisionBody() noexcept;

        slice getRevisionBody() noexcept;

        alloc_slice bodyAsJSON(bool canonical =false);

        FLDict getProperties() noexcept;

        alloc_slice getSelectedRevIDGlobalForm();

        alloc_slice getRevisionHistory(unsigned maxHistory,
                                          const C4String backToRevs[C4NULLABLE], // only if count=0
                                          unsigned backToRevsCount);

        // Remote database revision tracking:

        alloc_slice getRemoteAncestor(C4RemoteID);

        void setRemoteAncestor(C4RemoteID, C4String revID);

        // Purging:

        bool removeRevisionBody() noexcept;

        int32_t purgeRevision(C4Slice revID);

        // Conflicts:

        void resolveConflict(C4String winningRevID,
                             C4String losingRevID,
                             FLDict C4NULLABLE mergedProperties,
                             C4RevisionFlags mergedFlags,
                             bool pruneLosingBranch =true);


        void resolveConflict(C4String winningRevID,
                             C4String losingRevID,
                             C4Slice mergedBody,
                             C4RevisionFlags mergedFlags,
                             bool pruneLosingBranch =true);


        // Updating & Saving:

        /** Adds a new revision to this document, saves it to the database, and returns a
            document instance that knows the new revision -- it may or may not be the same instance
            as this one.
            If the database already contains a conflicting revision, returns nullptr. */
        Retained<C4Document> update(slice revBody, C4RevisionFlags);

        /** Saves changes to the document. Returns false on conflict. */
        bool save(unsigned maxRevTreeDepth =0);

    protected:
        C4Document() = default;
        ~C4Document() = default;
    };


// These declarations allow `Retained<C4Document>` to work.
C4Document* C4NULLABLE retain(C4Document* C4NULLABLE);
void release(C4Document* C4NULLABLE);

C4_ASSUME_NONNULL_END
