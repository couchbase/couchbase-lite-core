//
// ReplicatorTypes.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include "ReplicatedRev.hh"
#include "fleece/Fleece.h"
#include "c4.hh"
#include "c4Private.h"
#include "access_lock.hh"
#include <chrono>
#include <functional>
#include <set>
#include <unordered_set>
#include <vector>

namespace litecore { namespace repl {
    using fleece::RefCounted;
    using fleece::Retained;
    class IncomingRev;

    // Operations on C4Progress objects:

    static inline bool operator== (const C4Progress &p1, const C4Progress &p2) {
        return p1.unitsCompleted == p2.unitsCompleted && p1.unitsTotal == p2.unitsTotal
            && p1.documentCount == p2.documentCount;
    }
    static inline bool operator!= (const C4Progress &p1, const C4Progress &p2) {
        return !(p1 == p2);
    }
    static inline C4Progress operator+ (const C4Progress &p1, const C4Progress &p2) {
        return C4Progress {p1.unitsCompleted + p2.unitsCompleted, p1.unitsTotal + p2.unitsTotal,
                           p1.documentCount + p2.documentCount};
    }
    static inline C4Progress operator- (const C4Progress &p1, const C4Progress &p2) {
        return C4Progress {p1.unitsCompleted - p2.unitsCompleted, p1.unitsTotal - p2.unitsTotal,
                           p1.documentCount - p2.documentCount};
    }
    static inline C4Progress& operator+= (C4Progress &p1, const C4Progress &p2) {
        p1 = p1 + p2;
        return p1;
    }


    /** Thread-safe counted set of doc IDs. Can store multiple of the same docID;
        e.g. two add("x") calls require two remove("x") calls before contains("x") returns false. */
    class DocIDMultiset {
    public:
        bool contains(const fleece::alloc_slice &docID) const;
        void add(const fleece::alloc_slice &docID);
        void remove(const fleece::alloc_slice &docID);
    private:
        using multiset = std::unordered_multiset<fleece::alloc_slice, fleece::sliceHash>;
        access_lock<multiset> _set;
    };

    
    /** A request by the peer to send a revision. */
    class RevToSend : public ReplicatedRev {
    public:
        alloc_slice     remoteAncestorRevID;        // Known ancestor revID (no-conflicts mode)
        unsigned        maxHistory {0};             // Max depth of rev history to send
        const uint64_t  bodySize {0};               // (Estimated) size of body
        int64_t         expiration {0};             // Time doc expires
        bool            noConflicts {false};        // Server is in no-conflicts mode
        bool            legacyAttachments {false};  // Add _attachments property when sending
        bool            deltaOK {false};            // Can send a delta
        int8_t          retryCount {0};             // Number of times this revision has been retried
        std::unique_ptr<std::set<alloc_slice>> ancestorRevIDs; // Known ancestor revIDs the peer already has

        RevToSend(const C4DocumentInfo &info);

        void addRemoteAncestor(slice revID);
        bool hasRemoteAncestor(slice revID) const;


        Dir dir() const override                    {return Dir::kPushing;}
        void trim() override;

        std::string historyString(C4Document*);
        
    protected:
        ~RevToSend() =default;
    };

    typedef std::vector<Retained<RevToSend>> RevToSendList;


    /** A revision to be added to the database, complete with body. */
    class RevToInsert : public ReplicatedRev {
    public:
        alloc_slice             historyBuf;             // Revision history (comma-delimited revIDs)
        fleece::Doc             doc;
        const bool              noConflicts {false};    // Server is in no-conflicts mode
        Retained<IncomingRev>   owner;                  // Object that's processing this rev
        alloc_slice             deltaSrc;
        alloc_slice             deltaSrcRevID;          // Source revision if body is a delta
        
        RevToInsert(IncomingRev* owner,
                    slice docID, slice revID,
                    slice historyBuf,
                    bool deleted,
                    bool noConflicts);

        Dir dir() const override                    {return Dir::kPulling;}
        void trim() override;
        void trimBody();

        std::vector<C4Slice> history();

        void notifyInserted();

    protected:
        ~RevToInsert();
    };


    /** Metadata of a blob to download. */
    struct PendingBlob {
        fleece::alloc_slice docID;
        fleece::alloc_slice docProperty;
        C4BlobKey key;
        uint64_t length;
        bool compressible;
    };

} }
