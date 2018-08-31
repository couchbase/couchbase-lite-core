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
#include "fleece/Fleece.h"
#include "fleece/slice.hh"
#include "c4.hh"
#include "c4Private.h"
#include "RefCounted.hh"
#include <chrono>
#include <functional>
#include <set>
#include <vector>

namespace litecore { namespace repl {
    using fleece::RefCounted;
    using fleece::Retained;

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

    
    /** Metadata of a document revision. */
    class Rev : public RefCounted {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        const alloc_slice docID;
        const alloc_slice revID;
        const C4SequenceNumber sequence;
        const uint64_t bodySize;
        C4RevisionFlags flags {0};
        bool noConflicts {false};

        bool deleted() const        {return (flags & kRevDeleted) != 0;}

    protected:
        template <class SLICE1, class SLICE2>
        Rev(SLICE1 docID_, SLICE2 revID_, C4SequenceNumber sequence_ =0, uint64_t bodySize_ =0)
        :docID(docID_), revID(revID_), sequence(sequence_), bodySize(bodySize_)
        { }

        ~Rev() =default;
    };


    /** A request by the peer to send a revision. */
    class RevToSend : public Rev {
    public:
        alloc_slice remoteAncestorRevID;            // Known ancestor revID (no-conflicts mode)
        unsigned maxHistory {0};                    // Max depth of rev history to send
        bool legacyAttachments {false};             // Add _attachments property when sending

        RevToSend(const C4DocumentInfo &info,
                  const alloc_slice &remoteAncestor);

        void addRemoteAncestor(slice revID);
        bool hasRemoteAncestor(slice revID) const;
        
    protected:
        ~RevToSend() =default;

    private:
        std::unique_ptr<std::set<alloc_slice>> ancestorRevIDs;    // Known ancestor revIDs the peer already has
    };

    typedef std::vector<Retained<RevToSend>> RevToSendList;


    /** A revision to be added to the database, complete with body. */
    class RevToInsert : public Rev {
    public:
        const alloc_slice historyBuf;
        alloc_slice body;
        std::function<void(C4Error)> onInserted;

        RevToInsert(slice docID_, slice revID_,
                    slice historyBuf_,
                    bool deleted,
                    bool noConflicts);

    protected:
        ~RevToInsert() =default;
    };

} }
