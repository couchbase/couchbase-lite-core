//
//  DBActor.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ReplActor.hh"
#include "c4Base.h"
#include "slice.hh"
#include <string>
#include <vector>

namespace litecore { namespace repl {
    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;
    class Pusher;

    
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
    };


    /** Actor that manages database access for the replicator. */
    class DBActor : public ReplActor {
    public:
        DBActor(blip::Connection *connection,
                Replicator*,
                C4Database *db,
                const websocket::Address &remoteAddress,
                Options options);

        using CheckpointCallback = std::function<void(alloc_slice checkpointID,
                                                      alloc_slice data,
                                                      C4Error err)>;

        void getCheckpoint(CheckpointCallback cb) {
            enqueue(&DBActor::_getCheckpoint, cb);
        }

        void setCheckpoint(const alloc_slice &data, std::function<void()> onComplete) {
            enqueue(&DBActor::_setCheckpoint, data, onComplete);
        }

        void getChanges(C4SequenceNumber since, unsigned limit, bool continuous, Pusher*);

        void findOrRequestRevs(Retained<blip::MessageIn> req,
                               std::function<void(std::vector<alloc_slice>)> callback) {
            enqueue(&DBActor::_findOrRequestRevs, req, callback);
        }

        void sendRevision(const RevRequest &request,
                          blip::MessageProgressCallback onProgress) {
            enqueue(&DBActor::_sendRevision, request, onProgress);
        }

        void insertRevision(std::shared_ptr<RevToInsert> rev);

    protected:
        virtual void activityLevelChanged(ActivityLevel level) override;

    private:
        void handleGetCheckpoint(Retained<blip::MessageIn>);
        void handleSetCheckpoint(Retained<blip::MessageIn>);
        bool getPeerCheckpointDoc(blip::MessageIn* request, bool getting,
                                  fleece::slice &checkpointID, c4::ref<C4RawDocument> &doc);

        slice effectiveRemoteCheckpointDocID();
        void _getCheckpoint(CheckpointCallback);
        void _setCheckpoint(alloc_slice data, std::function<void()> onComplete);
        void _getChanges(C4SequenceNumber since, unsigned limit, bool continuous,
                         Retained<Pusher>);
        void _findOrRequestRevs(Retained<blip::MessageIn> req,
                                std::function<void(std::vector<alloc_slice>)> callback);
        void _sendRevision(RevRequest request,
                           blip::MessageProgressCallback onProgress);
        void _insertRevision(std::shared_ptr<RevToInsert> rev);


        void insertRevisionsNow()   {enqueue(&DBActor::_insertRevisionsNow);}
        void _insertRevisionsNow();

            void dbChanged();

        bool findAncestors(slice docID, slice revID,
                           std::vector<alloc_slice> &ancestors);

        static const size_t kMaxPossibleAncestors = 10;

        C4Database* const _db;
        const websocket::Address _remoteAddress;
        std::string _remoteCheckpointDocID;
        c4::ref<C4DatabaseObserver> _changeObserver;
        Retained<Pusher> _pusher;
        std::unique_ptr<std::vector<std::shared_ptr<RevToInsert>>> _revsToInsert;
        std::mutex _revsToInsertMutex;
        Timer _insertTimer;
        bool _insertDocumentMetadata {true}; //FIX: Currently set to true to accomodate SG
    };

} }
