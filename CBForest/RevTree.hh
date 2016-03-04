//
//  RevTree.hh
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__RevTree__
#define __CBForest__RevTree__

#include "slice.hh"
#include "RevID.hh"
#include "Database.hh"
#include <vector>


namespace cbforest {

    class RevTree;
    class RawRevision;

    /** In-memory representation of a single revision's metadata. */
    class Revision {
    public:
        const RevTree*  owner;
        revid           revID;      /**< Revision ID (compressed) */
        fdb_seqnum_t    sequence;   /**< DB sequence number that this revision has/had */

        inline slice inlineBody() const;     /**< Body if stored in Revision, or null if not */
        inline bool isBodyAvailable() const; /**< Is body inline or loadable from an earlier doc? */
        inline alloc_slice readBody() const; /**< Reads body from earlier doc if necessary */

        bool isLeaf() const         {return (flags & kLeaf) != 0;}
        bool isDeleted() const      {return (flags & kDeleted) != 0;}
        bool hasAttachments() const {return (flags & kHasAttachments) != 0;}
        bool isNew() const          {return (flags & kNew) != 0;}
        bool isActive() const       {return isLeaf() && !isDeleted();}

        unsigned index() const;
        const Revision* parent() const;
        const Revision* next() const;       // next by order in array, i.e. descending priority
        std::vector<const Revision*> history() const;

        bool operator< (const Revision& rev) const;

        enum Flags : uint8_t {
            kDeleted        = 0x01, /**< Is this revision a deletion/tombstone? */
            kLeaf           = 0x02, /**< Is this revision a leaf (no children?) */
            kNew            = 0x04, /**< Has this rev been inserted since decoding? */
            kHasAttachments = 0x08  /**< Does this rev's body contain attachments? */
        };
        Flags flags;

    private:
        static const uint16_t kNoParent = UINT16_MAX;
        
        slice       body;           /**< Revision body (JSON), or empty if not stored in this tree*/
        uint64_t    oldBodyOffset;  /**< File offset of doc containing revision body, or else 0 */
        uint16_t    parentIndex;    /**< Index in tree's rev[] array of parent revision, if any */

        void read(const RawRevision *src);
        RawRevision* write(RawRevision* dst, uint64_t bodyOffset) const;
        size_t sizeToWrite() const;
        void addFlag(Flags f)      {flags = (Flags)(flags | f);}
        void clearFlag(Flags f)    {flags = (Flags)(flags & ~f);}
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
        RevTree(slice raw_tree, sequence seq, uint64_t docOffset);
        virtual ~RevTree() { }

        void decode(slice raw_tree, sequence seq, uint64_t docOffset);

        alloc_slice encode();

        size_t size() const                             {return _revs.size();}
        const Revision* get(unsigned index) const;
        const Revision* get(revid) const;
        const Revision* operator[](unsigned index) const {return get(index);}
        const Revision* operator[](revid revID) const    {return get(revID);}
        const Revision* getBySequence(sequence) const;

#ifdef __OBJC__
        const Revision* get(NSString* revID) const;
#endif

        const std::vector<Revision>& allRevisions() const    {return _revs;}
        const Revision* currentRevision();
        std::vector<const Revision*> currentRevisions() const;
        bool hasConflict() const;

        const Revision* insert(revid, slice body,
                               bool deleted, bool hasAttachments,
                               revid parentRevID,
                               bool allowConflict,
                               int &httpStatus);
        const Revision* insert(revid, slice body,
                               bool deleted, bool hasAttachments,
                               const Revision* parent,
                               bool allowConflict,
                               int &httpStatus);
        int insertHistory(const std::vector<revidBuffer> history,
                          slice body,
                          bool deleted, bool hasAttachments);

        unsigned prune(unsigned maxDepth);

        /** Removes a leaf revision and any of its ancestors that aren't shared with other leaves. */
        int purge(revid);

        void sort();

#if DEBUG
        std::string dump();
#endif

    protected:
        virtual bool isBodyOfRevisionAvailable(const Revision*, uint64_t atOffset) const;
        virtual alloc_slice readBodyOfRevision(const Revision*, uint64_t atOffset) const;
#if DEBUG
        virtual void dump(std::ostream&);
#endif

    private:
        friend class Revision;
        const Revision* _insert(revid, slice body, const Revision *parentRev,
                                bool deleted, bool hasAttachments);
        bool confirmLeaf(Revision* testRev);
        void compact();
        RevTree(const RevTree&) = delete;

        uint64_t    _bodyOffset {0};     // File offset of body this tree was read from
        bool        _sorted {true};         // Are the revs currently sorted?
        std::vector<Revision> _revs;
        std::vector<alloc_slice> _insertedData;
    protected:
        bool _changed {false};
        bool _unknown {false};
    };


    inline bool Revision::isBodyAvailable() const {
        return owner->isBodyOfRevisionAvailable(this, oldBodyOffset);
    }
    inline alloc_slice Revision::readBody() const {
        return owner->readBodyOfRevision(this, oldBodyOffset);
    }
    inline slice Revision::inlineBody() const {
        return body;
    }

}

#endif /* defined(__CBForest__RevTree__) */
