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
#include "FleeceCpp.hh"
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
                const fleece::alloc_slice &remoteURL,
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

        void checkpointIsInvalid() {
            enqueue(&DBWorker::_checkpointIsInvalid);
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

        void sendRevision(RevToSend *request,
                          blip::MessageProgressCallback onProgress) {
            enqueue(&DBWorker::_sendRevision, retained(request), onProgress);
        }

        void insertRevision(RevToInsert *rev);

        void markRevSynced(Rev *rev);

        void setCookie(slice setCookieHeader) {
            enqueue(&DBWorker::_setCookie, alloc_slice(setCookieHeader));
        }

    protected:
        virtual std::string loggingClassName() const override {return "DBWorker";}

    private:
        std::string remoteDBIDString() const;
        void handleGetCheckpoint(Retained<blip::MessageIn>);
        void handleSetCheckpoint(Retained<blip::MessageIn>);
        void _checkpointIsInvalid();
        bool getPeerCheckpointDoc(blip::MessageIn* request, bool getting,
                                  fleece::slice &checkpointID, c4::ref<C4RawDocument> &doc);

        slice effectiveRemoteCheckpointDocID(C4Error*);
        void _getCheckpoint(CheckpointCallback);
        void _setCheckpoint(alloc_slice data, std::function<void()> onComplete);
        void _getChanges(GetChangesParams, Retained<Pusher> pusher);
        bool addChangeToList(const C4DocumentInfo &info, C4Document *doc,
                             std::shared_ptr<RevToSendList> &changes);
        void _findOrRequestRevs(Retained<blip::MessageIn> req,
                                std::function<void(std::vector<bool>)> callback);
        void _sendRevision(Retained<RevToSend> request,
                           blip::MessageProgressCallback onProgress);
        void _insertRevision(RevToInsert *rev);
        void _setCookie(alloc_slice setCookieHeader);

        void _markRevsSyncedNow();
        void _insertRevisionsNow();
        void _connectionClosed() override;

        void dbChanged();
        void _markRevSynced(Rev);

        fleeceapi::Dict getRevToSend(C4Document*, const RevToSend&, C4Error *outError);
        static std::string revHistoryString(C4Document*, const RevToSend&);
        void writeRevWithLegacyAttachments(fleeceapi::Encoder&,
                                           fleeceapi::Dict rev,
                                           FLSharedKeys sk);
        bool findAncestors(slice docID, slice revID,
                           std::vector<alloc_slice> &ancestors);
        int findProposedChange(slice docID, slice revID, slice parentRevID,
                               alloc_slice &outCurrentRevID);
        void updateRemoteRev(C4Document* NONNULL);
        ActivityLevel computeActivityLevel() const override;

        template <class REV>
        using Queue = std::unique_ptr<std::vector<Retained<REV>>>;
        template <class REV>
        void scheduleRevision(REV*, Queue<REV>&);
        template <class REV>
        Queue<REV> popScheduledRevisions(Queue<REV>&);

        static const size_t kMaxPossibleAncestors = 10;

        c4::ref<C4Database> _db;
        C4BlobStore* _blobStore;
        const websocket::URL _remoteURL;
        std::string _remoteCheckpointDocID;                 // docID of checkpoint
        C4RemoteID _remoteDBID {0};                         // ID # of remote DB in revision store
        bool _checkpointValid {true};
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        Retained<Pusher> _pusher;                           // Pusher to send db changes to
        DocIDSet _pushDocIDs;                               // Optional set of doc IDs to push
        C4SequenceNumber _maxPushedSequence {0};            // Latest seq that's been pushed
        bool _getForeignAncestors {false};
        bool _skipForeignChanges {false};
        
        Queue<RevToInsert> _revsToInsert; // Pending revs to be added to db
        Queue<Rev> _revsToMarkSynced; // Pending revs to be marked as synced
        bool _insertionScheduled {false};                   // True if call to insert/sync pending
        std::mutex _insertionQueueMutex;                    // For safe access to the above
    };

} }
