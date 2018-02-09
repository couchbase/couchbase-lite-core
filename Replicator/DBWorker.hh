//
// DBWorker.hh
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
#include "ReplicatorTypes.hh"
#include "Worker.hh"
#include "c4BlobStore.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace litecore { namespace repl {
    class Pusher;
    using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

    
    /** Actor that manages database access for the replicator. */
    class DBWorker : public Worker {
    public:
        DBWorker(blip::Connection *connection,
                Replicator*,
                C4Database *db,
                const websocket::Address &remoteAddress,
                Options options);

        /** The blob store is thread-safe so it can be accessed directly. */
        C4BlobStore* blobStore() const                  {return _blobStore;}

        using CheckpointCallback = std::function<void(alloc_slice checkpointID,
                                                      alloc_slice data,
                                                      bool dbIsEmpty,
                                                      C4Error err)>;

        void getCheckpoint(CheckpointCallback cb) {
            enqueue(&DBWorker::_getCheckpoint, cb);
        }

        void setCheckpoint(const alloc_slice &data, std::function<void()> onComplete) {
            enqueue(&DBWorker::_setCheckpoint, data, onComplete);
        }

        struct GetChangesParams {
            C4SequenceNumber since;
            DocIDSet docIDs;
            unsigned limit;
            bool continuous, getForeignAncestors;
            bool skipDeleted, skipForeign;
        };

        void getChanges(const GetChangesParams&, Pusher*);

        void findOrRequestRevs(Retained<blip::MessageIn> req,
                               std::function<void(std::vector<bool>)> callback) {
            enqueue(&DBWorker::_findOrRequestRevs, req, callback);
        }

        void sendRevision(const RevRequest &request,
                          blip::MessageProgressCallback onProgress) {
            enqueue(&DBWorker::_sendRevision, request, onProgress);
        }

        void insertRevision(RevToInsert *rev);

        void setCookie(slice setCookieHeader) {
            enqueue(&DBWorker::_setCookie, alloc_slice(setCookieHeader));
        }

    protected:
        virtual std::string loggingClassName() const override {return "DBWorker";}

    private:
        std::string remoteDBIDString() const;
        void handleGetCheckpoint(Retained<blip::MessageIn>);
        void handleSetCheckpoint(Retained<blip::MessageIn>);
        bool getPeerCheckpointDoc(blip::MessageIn* request, bool getting,
                                  fleece::slice &checkpointID, c4::ref<C4RawDocument> &doc);

        slice effectiveRemoteCheckpointDocID(C4Error*);
        void _getCheckpoint(CheckpointCallback);
        void _setCheckpoint(alloc_slice data, std::function<void()> onComplete);
        void _getChanges(GetChangesParams, Retained<Pusher> pusher);
        bool addChangeToList(const C4DocumentInfo &info, C4Document *doc, std::vector<Rev> &changes);
        alloc_slice getRemoteRevID(C4Document*);
        void _findOrRequestRevs(Retained<blip::MessageIn> req,
                                std::function<void(std::vector<bool>)> callback);
        void _sendRevision(RevRequest request,
                           blip::MessageProgressCallback onProgress);
        void _insertRevision(RevToInsert *rev);
        void _setCookie(alloc_slice setCookieHeader);

        void insertRevisionsNow()   {enqueue(&DBWorker::_insertRevisionsNow);}
        void _insertRevisionsNow();
        void _connectionClosed() override;

        void dbChanged();
        bool markRevsSynced(const std::vector<Rev> changes, C4Error *outError);

        void writeRevWithLegacyAttachments(fleeceapi::Encoder&,
                                           fleeceapi::Dict rev,
                                           FLSharedKeys sk);
        bool findAncestors(slice docID, slice revID,
                           std::vector<alloc_slice> &ancestors);
        int findProposedChange(slice docID, slice revID, slice parentRevID);
        void updateRemoteRev(C4Document* NONNULL);

        static const size_t kMaxPossibleAncestors = 10;

        c4::ref<C4Database> _db;
        C4BlobStore* _blobStore;
        const websocket::Address _remoteAddress;
        std::string _remoteCheckpointDocID;                 // docID of checkpoint
        C4RemoteID _remoteDBID {0};                         // ID # of remote DB in revision store
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        Retained<Pusher> _pusher;                           // Pusher to send db changes to
        DocIDSet _pushDocIDs;                               // Optional set of doc IDs to push
        bool _getForeignAncestors {false};
        bool _skipForeignChanges {false};
        std::unique_ptr<std::vector<RevToInsert*>> _revsToInsert; // Pending revs to be added to db
        std::mutex _revsToInsertMutex;                      // For safe access to _revsToInsert
        actor::Timer _insertTimer;                          // Timer for inserting revs
        C4SequenceNumber _maxPushedSequence {0};            // Latest seq that's been pushed
    };

} }
