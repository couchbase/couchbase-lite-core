//
//  ReplicatorTypes.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/24/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Fleece.h"
#include "slice.hh"
#include "c4.hh"
#include "c4Replicator.h"
#include <chrono>
#include <functional>
#include <vector>

namespace litecore { namespace repl {
    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;

    
    static inline bool operator== (const C4Progress &p1, const C4Progress &p2) {
        return p1.completed == p2.completed && p1.total == p2.total;
    }
    static inline bool operator!= (const C4Progress &p1, const C4Progress &p2) {
        return !(p1 == p2);
    }
    static inline C4Progress operator+ (const C4Progress &p1, const C4Progress &p2) {
        return C4Progress {p1.completed + p2.completed, p1.total + p2.total};
    }

    
    /** Metadata of a document revision. */
    struct Rev {
        alloc_slice docID;
        alloc_slice revID;
        C4SequenceNumber sequence {0};

        Rev() { }

        Rev(slice d, slice r, C4SequenceNumber s)
        :docID(d), revID(r), sequence(s)
        { }

        Rev(const C4DocumentInfo &info)
        :Rev(info.docID, info.revID, info.sequence)
        { }
    };

    typedef std::vector<Rev> RevList;


    /** A request by the peer to send a revision. */
    struct RevRequest : public Rev {
        std::vector<alloc_slice> ancestorRevIDs;    // Known ancestor revIDs the peer already has
        unsigned maxHistory;                        // Max depth of rev history to send

        RevRequest(const Rev &rev, unsigned maxHistory_)
        :Rev(rev)
        ,maxHistory(maxHistory_)
        { }
    };


    /** A revision I want from the peer; includes the opaque remote revision ID. */
    struct RequestedRev : public Rev {
        alloc_slice remoteSequence;

        RequestedRev() { }
    };


    struct RevToInsert : public Rev {
        bool deleted {false};
        alloc_slice historyBuf;
        alloc_slice body;
        std::function<void(C4Error)> onInserted;

        void clear() {
            docID = revID = historyBuf = body = fleece::nullslice;
            onInserted = nullptr;
        }
    };

} }
