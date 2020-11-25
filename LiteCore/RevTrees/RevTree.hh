//
// RevTree.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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
#include "PlatformCompat.hh"
#include "fleece/slice.hh"
#include "RevID.hh"
#include <climits>
#include <deque>
#include <unordered_map>
#include <vector>


namespace litecore {

    class RevTree;

    /** In-memory representation of a single revision's metadata. */
    class Rev {
    public:
        const RevTree*  owner;
        const Rev*      parent;
        revid           revID;      /**< Revision ID (compressed) */
        sequence_t      sequence;   /**< DB sequence number that this revision has/had */

        slice body() const;
        bool isBodyAvailable() const{return _body.buf != nullptr;}

        bool isLeaf() const         {return (flags & kLeaf) != 0;}
        bool isDeleted() const      {return (flags & kDeleted) != 0;}
        bool hasAttachments() const {return (flags & kHasAttachments) != 0;}
        bool isNew() const          {return (flags & kNew) != 0;}
        bool isConflict() const     {return (flags & kIsConflict) != 0;}
        bool isClosed() const       {return (flags & kClosed) != 0;}
        bool keepBody() const       {return (flags & kKeepBody) != 0;}
        bool isActive() const;

        unsigned index() const;
        const Rev* next() const;       // next by order in array, i.e. descending priority
        std::vector<const Rev*> history() const;
        bool isAncestorOf(const Rev* NONNULL) const;
        bool isLatestRemoteRevision() const;

        bool operator< (const Rev& rev) const;

        enum Flags : uint8_t {
            kNoFlags        = 0x00,
            kDeleted        = 0x01, /**< Is this revision a deletion/tombstone? */
            kLeaf           = 0x02, /**< Is this revision a leaf (no children?) */
            kNew            = 0x04, /**< Has this rev been inserted since decoding? */
            kHasAttachments = 0x08, /**< Does this rev's body contain attachments? */
            kKeepBody       = 0x10, /**< Body will not be discarded after I'm a non-leaf */
            kIsConflict     = 0x20, /**< Unresolved conflicting revision; should never be current */
            kClosed         = 0x40, /**< Rev is the end of a closed conflicting branch */
            // Keep these flags consistent with C4RevisionFlags, in c4Document.h!
            kPurge          = 0x80, /**< (Internal: Rev is marked for purging/pruning) */
        };
        Flags flags;

    private:
        slice       _body;          /**< Revision body (JSON), or empty if not stored in this tree*/

        void addFlag(Flags f)           {flags = (Flags)(flags | f);}
        void clearFlag(Flags f)         {flags = (Flags)(flags & ~f);}
        void removeBody()               {clearFlag((Flags)(kKeepBody | kHasAttachments));
                                         _body = nullslice;}
        bool isMarkedForPurge() const   {return (flags & kPurge) != 0;}
#if DEBUG
        void dump(std::ostream&);
#endif
        friend class RevTree;
        friend class RawRevision;
    };


    /** A serializable tree of Revisions. */
    class RevTree {
    public:
        RevTree() { }
        RevTree(slice raw_tree, sequence_t seq);
        RevTree(const RevTree&);
        virtual ~RevTree() { }

        void decode(slice raw_tree, sequence_t seq);

        alloc_slice encode();

        size_t size() const                             {return _revs.size();}
        const Rev* get(unsigned index) const;
        const Rev* get(revid) const;
        const Rev* operator[](unsigned index) const {return get(index);}
        const Rev* operator[](revid revID) const    {return get(revID);}
        const Rev* getBySequence(sequence_t) const;

        const std::vector<Rev*>& allRevisions() const   {return _revs;}
        const Rev* currentRevision();
        bool hasConflict() const;
        bool hasNewRevisions() const;

        /// For unknown reason, sometimes the rev tree would come into an inconsistent state,
        /// in which some conflicted leaf revs are not tagged conflict.
        /// The method is a workaround for the problem. It tag those unclosed, non-current
        /// leaves as conflict, solving the problem regardless of its real cause.
        ///
        /// Return true if it was recovered from inconsistent state
        /// Return false otherwise
        bool ensureConflictStateConsistent();

        /// Given an array of revision IDs in consecutive descending-generation order,
        /// finds the first one that exists in this tree. Returns:
        /// * {rev, index} if a common ancestor was found;
        /// * {nullptr, n} , where n=history.size(), if there are no common revisions;
        /// * {nullptr, -400} if the history array is invalid
        /// * {nullptr, -409} if `allowConflict` is false and inserting would cause a conflict
        std::pair<Rev*,int> findCommonAncestor(const std::vector<revidBuffer> history,
                                               bool allowConflict);

        // Adds a new leaf revision, given the parent's revID
        const Rev* insert(revid,
                          alloc_slice body,
                          Rev::Flags,
                          revid parentRevID,
                          bool allowConflict,
                          bool markConflict,
                          int &httpStatus);

        // Adds a new leaf revision, given a pointer to the parent Rev
        const Rev* insert(revid,
                          alloc_slice body,
                          Rev::Flags,
                          const Rev* parent,
                          bool allowConflict,
                          bool markConflict,
                          int &httpStatus);

        // Adds a new leaf revision along with any new ancestor revs in its history.
        // (history[0] is the new rev's ID, history[1] is its parent's, etc.)
        // Returns the index in `history` of the common ancestor,
        // or -400 if the history vector is invalid, or -409 if there would be a conflict.
        int insertHistory(const std::vector<revidBuffer> history,
                          alloc_slice body,
                          Rev::Flags,
                          bool allowConflict,
                          bool markConflict);

        // Clears the kIsConflict flag for a Rev and its ancestors.
        void markBranchAsNotConflict(const Rev*, bool keepBodies);
        
        // CBL-1089 / CBL-1174: Reset the sequence so that it can be the latest
        // when saved
        void resetConflictSequence(const Rev*);

        void setPruneDepth(unsigned depth)              {_pruneDepth = depth;}
        unsigned prune(unsigned maxDepth);
        unsigned prune()                                {return prune(_pruneDepth);}

        void keepBody(const Rev* NONNULL);
        void removeBody(const Rev* NONNULL);
        void removeBodiesOnBranch(const Rev* NONNULL);

        void removeNonLeafBodies();

        /** Removes a leaf revision and any of its ancestors that aren't shared with other leaves. */
        int purge(revid);
        int purgeAll();

        void sort();

        void saved(sequence_t newSequence);

        //////// Remotes:

        using RemoteID = unsigned;
        static constexpr RemoteID kNoRemoteID = 0;
        static constexpr RemoteID kDefaultRemoteID = 1;     // 1st (& usually only) remote server

        const Rev* latestRevisionOnRemote(RemoteID);
        void setLatestRevisionOnRemote(RemoteID, const Rev*);

#if DEBUG
        void dump();
#endif

    protected:
        virtual bool isBodyOfRevisionAvailable(const Rev* r NONNULL) const;
        bool isLatestRemoteRevision(const Rev* NONNULL) const;
        virtual alloc_slice readBodyOfRevision(const Rev* r NONNULL) const;
        virtual alloc_slice copyBody(slice body);
        virtual alloc_slice copyBody(const alloc_slice &body);
#if DEBUG
        virtual void dump(std::ostream&);
#endif

        bool _changed {false};
        bool _unknown {false};

    private:
        friend class Rev;
        friend class RawRevision;
        void initRevs();
        Rev* _insert(revid, alloc_slice body, Rev *parentRev, Rev::Flags, bool markConflicts);
        bool confirmLeaf(Rev* testRev NONNULL);
        void compact();
        void checkForResolvedConflict();

        using RemoteRevMap = std::unordered_map<RemoteID, const Rev*>;

        bool                     _sorted {true};        // Is _revs currently sorted?
        std::vector<Rev*>        _revs;                 // Revs in sorted order
        std::deque<Rev>          _revsStorage;          // Actual storage of the Rev objects
        std::vector<alloc_slice> _insertedData;         // Storage for new revids
        RemoteRevMap             _remoteRevs;           // Tracks current rev for a remote DB URL
        unsigned                 _pruneDepth {UINT_MAX};// Tree depth to prune to
    };

}
