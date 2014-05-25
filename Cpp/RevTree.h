//
//  RevTree.h
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__RevTree__
#define __CBForest__RevTree__

#include "slice.h"
#include "Database.h"
#include <vector>


namespace forestdb {

    /** In-memory representation of a single revision's metadata. */
    struct RevNode {

        enum {
            kDeleted = 0x01,    /**< Is this revision a deletion/tombstone? */
            kLeaf    = 0x02,    /**< Is this revision a leaf (no children?) */
            kNew     = 0x04     /**< Has this node been inserted since decoding? */
        };
        typedef uint8_t Flags;

        static const uint16_t kNoParent = UINT16_MAX;

        slice       revID;          /**< Revision ID */
        slice       data;           /**< Revision body (JSON), or empty if not stored in this tree */
        uint64_t    oldBodyOffset;  /**< File offset of doc containing revision body, or else 0 */
        fdb_seqnum_t sequence;      /**< DB sequence number that this revision has/had */
        uint16_t    parentIndex;    /**< Index in tree's node[] array of parent revision, if any */
        Flags       flags;          /**< Leaf/deleted flags */

        bool isLeaf() const    {return (flags & kLeaf) != 0;}
        bool isDeleted() const {return (flags & kDeleted) != 0;}
        bool isNew() const     {return (flags & kNew) != 0;}
        bool isActive() const  {return isLeaf() && !isDeleted();}

        int compare(const RevNode&) const;
        bool operator< (const RevNode& rev) const {return compare(rev) < 0;}
    };


    class RevTree {
    public:
        RevTree();
        RevTree(slice raw_tree, sequence seq, uint64_t docOffset);

        void decode(slice raw_tree, sequence seq, uint64_t docOffset);

        alloc_slice encode();

        size_t size() const                             {return _nodes.size();}
        const RevNode* get(unsigned index) const;
        const RevNode* get(slice revID) const;
        const RevNode* operator[](unsigned index) const {return get(index);}
        const RevNode* operator[](slice revID) const    {return get(revID);}
        unsigned indexOf(const RevNode* node) const;

        const RevNode* parentNode(const RevNode* node) const;

        const RevNode* currentNode();
        std::vector<const RevNode*> currentNodes();
        bool hasConflict() const;

        const RevNode* insert(slice revID, slice body, bool deleted, slice parentRevID, bool allowConflict);
        const RevNode* insert(slice revID, slice body, bool deleted, const RevNode* parent, bool allowConflict);
        int insertHistory(const std::vector<slice> history, slice data, bool deleted);

        unsigned prune(unsigned maxDepth);
        unsigned purge(const std::vector<slice>revIDs);

        void sort();

    private:
        const RevNode* _insert(slice revID, slice data, const RevNode *parentNode, bool deleted);
        void compact();

        uint64_t    _bodyOffset;     // File offset of body this tree was read from
        bool        _sorted;         // Are the nodes currently sorted?
        std::vector<RevNode> _nodes;
        std::vector<alloc_slice> _insertedData;
    protected:
        bool _changed;
    };

    bool RevIDParse(slice rev, unsigned *generation, slice *digest);
    bool RevIDParseCompacted(slice rev, unsigned *generation, slice *digest);


}

#endif /* defined(__CBForest__RevTree__) */
