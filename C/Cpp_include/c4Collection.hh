//
// c4Collection.hh
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
#include "c4DatabaseTypes.h"
#include "c4DocumentTypes.h"
#include "c4IndexTypes.h"
#include "c4QueryTypes.h"
#include "access_lock.hh"
#include "function_ref.hh"
#include <functional>
#include <memory>
#include <unordered_set>

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


struct C4Collection : public fleece::InstanceCountedIn<C4Collection>, C4Base {
    // Accessors:
    
    slice getName() const                                   {return _name;}

    C4Database* getDatabase();
    const C4Database* getDatabase() const;

    virtual uint64_t getDocumentCount() const =0;

    virtual C4SequenceNumber getLastSequence() const =0;

    C4ExtraInfo& extraInfo()                                {return _extraInfo;}
    const C4ExtraInfo& extraInfo() const                    {return _extraInfo;}

    // Documents:

    static C4Document* documentContainingValue(FLValue) noexcept;

    virtual Retained<C4Document> getDocument(slice docID,
                                             bool mustExist = true,
                                             C4DocContentLevel content = kDocGetCurrentRev) const =0;

    virtual Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const =0;

    virtual Retained<C4Document> putDocument(const C4DocPutRequest &rq,
                                             size_t* C4NULLABLE outCommonAncestorIndex,
                                             C4Error *outError) =0;

    virtual Retained<C4Document> createDocument(slice docID,
                                                slice revBody,
                                                C4RevisionFlags revFlags,
                                                C4Error *outError) =0;

    // Moves a document to another collection
    virtual void moveDocument(slice docID,
                              C4Collection *toCollection,
                              slice newDocID = fleece::nullslice) =0;

    // Purging & Expiration:

    virtual bool purgeDocument(slice docID) =0;

    virtual bool setExpiration(slice docID, C4Timestamp timestamp) =0;
    virtual C4Timestamp getExpiration(slice docID) const =0;

    virtual C4Timestamp nextDocExpiration() const =0;
    virtual int64_t purgeExpiredDocs() =0;

    // Indexes:

    virtual void createIndex(slice name,
                             slice indexSpecJSON,
                             C4IndexType indexType,
                             const C4IndexOptions* C4NULLABLE indexOptions =nullptr) =0;

    virtual void deleteIndex(slice name) =0;

    virtual alloc_slice getIndexesInfo(bool fullInfo = true) const =0;

    virtual alloc_slice getIndexRows(slice name) const =0;

    // Observers:

    using CollectionObserverCallback = std::function<void(C4CollectionObserver*)>;
    using DocumentObserverCallback = std::function<void(C4DocumentObserver*,
                                                        slice docID,
                                                        C4SequenceNumber)>;

    virtual std::unique_ptr<C4CollectionObserver> observe(CollectionObserverCallback) =0;

    virtual std::unique_ptr<C4DocumentObserver> observeDocument(slice docID,
                                                                DocumentObserverCallback) =0;

    // Internal use only:

    virtual ~C4Collection() = default;

    virtual std::vector<alloc_slice> findDocAncestors(const std::vector<slice> &docIDs,
                                                      const std::vector<slice> &revIDs,
                                                      unsigned maxAncestors,
                                                      bool mustHaveBodies,
                                                      C4RemoteID remoteDBID) const =0;
    virtual bool markDocumentSynced(slice docID,
                                    slice revID,
                                    C4SequenceNumber sequence,
                                    C4RemoteID remoteID) =0;

    virtual void findBlobReferences(const fleece::function_ref<bool(FLDict)>&) =0;

protected:
    C4Collection(C4Database*, slice name);

    C4Database* C4NULLABLE  _database;
    alloc_slice             _name;
    C4ExtraInfo             _extraInfo = {};
};


/** Flags produced by \ref C4Collection::findDocAncestors, the result of comparing a local document's
    revision(s) against the requested revID. */
typedef C4_OPTIONS(uint8_t, C4FindDocAncestorsResultFlags) {
    kRevsSame           = 0,    // Current revision is equal
    kRevsLocalIsOlder   = 1,    // Current revision is older
    kRevsLocalIsNewer   = 2,    // Current revision is newer
    kRevsConflict       = 3,    // Current revision conflicts (== LocalIsOlder | LocalIsNewer)
    kRevsAtThisRemote   = 4,    // The given C4RemoteID has this revID
    kRevsHaveLocal      = 8,    // Local doc has this revID with its body
};


C4_ASSUME_NONNULL_END
