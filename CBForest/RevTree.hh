//
//  RevTree.hh
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__RevTree__
#define __CBForest__RevTree__

#include "slice.hh"
#include "RevID.hh"
#include "Database.hh"
#include <vector>


namespace forestdb {

    class RevTree;

    /** In-memory representation of a single revision's metadata. */
    struct RevNode {

        enum {
            kDeleted = 0x01,    /**< Is this revision a deletion/tombstone? */
            kLeaf    = 0x02,    /**< Is this revision a leaf (no children?) */
            kNew     = 0x04     /**< Has this node been inserted since decoding? */
        };
        typedef uint8_t Flags;

        static const uint16_t kNoParent = UINT16_MAX;

        const RevTree* owner;
        revid       revID;          /**< Revision ID (compressed) */
        slice       body;           /**< Revision body (JSON), or empty if not stored in this tree */
        uint64_t    oldBodyOffset;  /**< File offset of doc containing revision body, or else 0 */
        fdb_seqnum_t sequence;      /**< DB sequence number that this revision has/had */
        uint16_t    parentIndex;    /**< Index in tree's node[] array of parent revision, if any */
        Flags       flags;          /**< Leaf/deleted flags */

        bool isLeaf() const    {return (flags & kLeaf) != 0;}
        bool isDeleted() const {return (flags & kDeleted) != 0;}
        bool isNew() const     {return (flags & kNew) != 0;}
        bool isActive() const  {return isLeaf() && !isDeleted();}

        unsigned index() const;
        const RevNode* parent() const;
        std::vector<const RevNode*> history() const;

        inline bool isBodyAvailable() const;
        inline alloc_slice readBody() const;

        bool operator< (const RevNode& rev) const;
    };


    /** A serializable tree of RevNodes. */
    class RevTree {
    public:
        RevTree();
        RevTree(slice raw_tree, sequence seq, uint64_t docOffset);
        virtual ~RevTree();

        void decode(slice raw_tree, sequence seq, uint64_t docOffset);

        alloc_slice encode();

        size_t size() const                             {return _nodes.size();}
        const RevNode* get(unsigned index) const;
        const RevNode* get(revid) const;
        const RevNode* operator[](unsigned index) const {return get(index);}
        const RevNode* operator[](revid revID) const    {return get(revID);}

        const std::vector<RevNode>& allNodes() const    {return _nodes;}
        const RevNode* currentNode();
        std::vector<const RevNode*> currentNodes() const;
        bool hasConflict() const;


        const RevNode* insert(revid, slice body, bool deleted,
                              revid parentRevID,
                              bool allowConflict,
                              int &httpStatus);
        const RevNode* insert(revid, slice body, bool deleted,
                              const RevNode* parent,
                              bool allowConflict,
                              int &httpStatus);
        int insertHistory(const std::vector<revid> history, slice body, bool deleted);

        unsigned prune(unsigned maxDepth);
        std::vector<revid> purge(const std::vector<revid>revIDs);

        void sort();

    protected:
        virtual bool isBodyOfNodeAvailable(const RevNode*) const;
        virtual alloc_slice readBodyOfNode(const RevNode*) const;

    private:
        friend struct RevNode;
        const RevNode* _insert(revid, slice body, const RevNode *parentNode, bool deleted);
        void compact();
        RevTree(const RevTree&); // forbidden

        uint64_t    _bodyOffset;     // File offset of body this tree was read from
        bool        _sorted;         // Are the nodes currently sorted?
        std::vector<RevNode> _nodes;
        std::vector<alloc_slice> _insertedData;
    protected:
        bool _changed;
        bool _unknown;
    };


    inline bool RevNode::isBodyAvailable() const {
        return owner->isBodyOfNodeAvailable(this);
    }
    inline alloc_slice RevNode::readBody() const {
        return owner->readBodyOfNode(this);
    }

}

#endif /* defined(__CBForest__RevTree__) */
