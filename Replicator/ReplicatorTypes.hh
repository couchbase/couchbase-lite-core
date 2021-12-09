//
// ReplicatorTypes.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "ReplicatedRev.hh"
#include "fleece/Fleece.hh"
#include "c4Base.hh"
#include "c4BlobStore.hh"
#include <memory>
#include <vector>

struct C4DocumentInfo;

namespace litecore { namespace repl {
    using fleece::RefCounted;
    using fleece::Retained;
    using fleece::RetainedConst;
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


    /** A request by the peer to send a revision. */
    class RevToSend final : public ReplicatedRev {
    public:
        alloc_slice     remoteAncestorRevID;        // Known ancestor revID (no-conflicts mode)
        unsigned        maxHistory {0};             // Max depth of rev history to send
        const uint64_t  bodySize {0};               // (Estimated) size of body
        int64_t         expiration {0};             // Time doc expires
        std::unique_ptr<std::vector<alloc_slice>> ancestorRevIDs; // Known ancestor revIDs the peer already has
        Retained<RevToSend> nextRev;                // Newer rev waiting for this one to finish
        bool            noConflicts {false};        // Server is in no-conflicts mode
        bool            legacyAttachments {false};  // Add _attachments property when sending
        bool            deltaOK {false};            // Can send a delta
        int8_t          retryCount {0};             // Number of times this revision has been retried

        RevToSend(const C4DocumentInfo &info);

        void addRemoteAncestor(slice revID);
        bool hasRemoteAncestor(slice revID) const;


        Dir dir() const override                    {return Dir::kPushing;}
        void trim() override;

        alloc_slice historyString(C4Document*);
        
    protected:
        ~RevToSend() =default;
    };

    typedef std::vector<Retained<RevToSend>> RevToSendList;


    /** A revision to be added to the database, complete with body. */
    class RevToInsert final : public ReplicatedRev {
    public:
        alloc_slice             historyBuf;             // Revision history (comma-delimited revIDs)
        fleece::Doc             doc;
        const bool              noConflicts {false};    // Server is in no-conflicts mode
        RevocationMode          revocationMode = RevocationMode::kNone;
        Retained<IncomingRev>   owner;                  // Object that's processing this rev
        alloc_slice             deltaSrc;
        alloc_slice             deltaSrcRevID;          // Source revision if body is a delta
        
        RevToInsert(IncomingRev* owner,
                    slice docID, slice revID,
                    slice historyBuf,
                    bool deleted,
                    bool noConflicts);

        /// Constructor for a revoked document
        RevToInsert(slice docID, slice revID,
                    RevocationMode);

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
