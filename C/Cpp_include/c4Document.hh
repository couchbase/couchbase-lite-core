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


namespace c4 {

    // This class declares the C++ API of C4Document.
    // In c4Document.h, C4Document is declared as inheriting from c4::DocumentAPI,
    // so it acquires this API.
    //
    // *** You should not refer to c4::DocumentAPI directly! Just use C4Document. ***
    //
    // (This convoluted setup is necessary to preserve the existing C API of C4Document, which
    // exposes the struct definition publicly.)
    class DocumentAPI : public C4Base {
    public:
        // NOTE: Instances are created with database->getDocument or database->putDocument.

        // Static utility functions:

        static constexpr size_t kGeneratedIDLength = 23;

        static char* generateID(char *outDocID,
                                size_t bufferSize) noexcept;

        static bool equalRevIDs(slice revID1,
                                slice revID2) noexcept;
        static unsigned getRevIDGeneration(slice revID) noexcept;

        static C4RevisionFlags currentRevFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept;

        /// Returns the Document instance, if any, that contains the given Fleece value.
        static C4Document* C4NULLABLE containing(FLValue) noexcept;

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

        bool loadSelectedRevBody(); // can throw; returns false if compacted away

        bool hasRevisionBody() noexcept;

        slice getSelectedRevBody() noexcept;

        alloc_slice bodyAsJSON(bool canonical =false);

        FLDict getSelectedRevRoot() noexcept;

        alloc_slice getSelectedRevIDGlobalForm();

        alloc_slice getSelectedRevHistory(unsigned maxHistory,
                                          const C4String backToRevs[C4NULLABLE], // only if count=0
                                          unsigned backToRevsCount);

        // Remote database revision tracking:

        alloc_slice remoteAncestorRevID(C4RemoteID);

        void setRemoteAncestorRevID(C4RemoteID, C4String revID);

        // Purging:

        bool removeSelectedRevBody() noexcept;

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
        DocumentAPI() = default;
        ~DocumentAPI() = default;
    };

}


// These declarations allow `Retained<C4Document>` to work.
c4::DocumentAPI* C4NULLABLE retain(c4::DocumentAPI* C4NULLABLE);
void release(c4::DocumentAPI* C4NULLABLE);
C4Document* C4NULLABLE retain(C4Document* C4NULLABLE);
void release(C4Document* C4NULLABLE);

C4_ASSUME_NONNULL_END
