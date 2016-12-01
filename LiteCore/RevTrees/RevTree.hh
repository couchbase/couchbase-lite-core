//
//  RevTree.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once
#include "slice.hh"
#include "RevID.hh"
#include "DataFile.hh"
#include <vector>


namespace litecore {

    class RevTree;

    /** In-memory representation of a single revision's metadata. */
    class Rev {
    public:
        const RevTree*  owner;
        revid           revID;      /**< Revision ID (compressed) */
        sequence_t      sequence;   /**< DB sequence number that this revision has/had */

        inline slice inlineBody() const;     /**< Body if stored in Revision, or null if not */
        inline bool isBodyAvailable() const; /**< Is body inline or loadable from an earlier record? */
        inline alloc_slice readBody() const; /**< Reads body from earlier record if necessary */

        bool isLeaf() const         {return (flags & kLeaf) != 0;}
        bool isDeleted() const      {return (flags & kDeleted) != 0;}
        bool hasAttachments() const {return (flags & kHasAttachments) != 0;}
        bool isNew() const          {return (flags & kNew) != 0;}
        bool isActive() const       {return isLeaf() && !isDeleted();}

        unsigned index() const;
        const Rev* parent() const;
        const Rev* next() const;       // next by order in array, i.e. descending priority
        std::vector<const Rev*> history() const;

        bool operator< (const Rev& rev) const;

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
        uint64_t    oldBodyOffset;  /**< File offset of record containing revision body, or else 0 */
        uint16_t    parentIndex;    /**< Index in tree's rev[] array of parent revision, if any */

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
        RevTree(slice raw_tree, sequence seq, uint64_t recordOffset);
        virtual ~RevTree() { }

        void decode(slice raw_tree, sequence seq, uint64_t recordOffset);

        alloc_slice encode();

        size_t size() const                             {return _revs.size();}
        const Rev* get(unsigned index) const;
        const Rev* get(revid) const;
        const Rev* operator[](unsigned index) const {return get(index);}
        const Rev* operator[](revid revID) const    {return get(revID);}
        const Rev* getBySequence(sequence) const;

        const std::vector<Rev>& allRevisions() const    {return _revs;}
        const Rev* currentRevision();
        std::vector<const Rev*> currentRevisions() const;
        bool hasConflict() const;

        const Rev* insert(revid, slice body,
                               bool deleted, bool hasAttachments,
                               revid parentRevID,
                               bool allowConflict,
                               int &httpStatus);
        const Rev* insert(revid, slice body,
                               bool deleted, bool hasAttachments,
                               const Rev* parent,
                               bool allowConflict,
                               int &httpStatus);
        int insertHistory(const std::vector<revidBuffer> history,
                          slice body,
                          bool deleted, bool hasAttachments);

        unsigned prune(unsigned maxDepth);

        /** Removes a leaf revision and any of its ancestors that aren't shared with other leaves. */
        int purge(revid);
        int purgeAll();

        void sort();

        void saved();

#if DEBUG
        std::string dump();
#endif

    protected:
        virtual bool isBodyOfRevisionAvailable(const Rev*, uint64_t atOffset) const;
        virtual alloc_slice readBodyOfRevision(const Rev*, uint64_t atOffset) const;
#if DEBUG
        virtual void dump(std::ostream&);
#endif

    private:
        friend class Rev;
        const Rev* _insert(revid, slice body, const Rev *parentRev,
                                bool deleted, bool hasAttachments);
        bool confirmLeaf(Rev* testRev);
        void compact();
        RevTree(const RevTree&) = delete;

        uint64_t    _bodyOffset {0};     // File offset of body this tree was read from
        bool        _sorted {true};         // Are the revs currently sorted?
        std::vector<Rev> _revs;
        std::vector<alloc_slice> _insertedData;
    protected:
        bool _changed {false};
        bool _unknown {false};
    };


    inline bool Rev::isBodyAvailable() const {
        return owner->isBodyOfRevisionAvailable(this, oldBodyOffset);
    }
    inline alloc_slice Rev::readBody() const {
        return owner->readBodyOfRevision(this, oldBodyOffset);
    }
    inline slice Rev::inlineBody() const {
        return body;
    }

}
