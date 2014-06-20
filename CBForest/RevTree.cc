//
//  RevTree.cc
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "RevTree.hh"
#include "varint.h"
#include <forestdb.h>
#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CFByteOrder.h>


#define offsetby(PTR,OFFSET) (void*)((uint8_t*)(PTR)+(OFFSET))


namespace forestdb {

    // Private RevNodeFlags bits:
    enum {
        kRevNodePublicPersistentFlags = (RevNode::kLeaf | RevNode::kDeleted),
        kRevNodeHasData = 0x80,    /**< Does this raw node contain JSON data? */
        kRevNodeHasBodyOffset = 0x40    /**< Does this raw node have a file position (oldBodyOffset)? */
    };
    
    // Layout of revision node in encoded form. Tree is a sequence of these followed by a 32-bit zero.
    // Nodes are stored in decending priority, with the current leaf node(s) coming first.
    struct RawRevNode {
        uint32_t        size;           // Total size of this tree node
        uint16_t        parentIndex;
        RevNode::Flags  flags;
        uint8_t         revIDLen;
        char            revID[1];       // actual size is [revIDLen]
        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];       // Contains the revision body (JSON)
        // else:
        //    varint    oldBodyOffset;  // Points to doc that has the body (0 if none)
        //    varint    body_size;
    };

    static size_t sizeForRawNode(const RevNode *node);
    static unsigned countRawNodes(const RawRevNode *tree);
    static void nodeFromRawNode(const RawRevNode *src, RevNode *dst);

    static inline bool validRawNode(const RawRevNode *rawNode)
    {
        return ntohl(rawNode->size) > 0;
    }

    static inline const RawRevNode *nextRawNode(const RawRevNode *node)
    {
        return (const RawRevNode*)offsetby(node, ntohl(node->size));
    }


    RevTree::RevTree()
    :_bodyOffset(0), _sorted(true), _changed(false), _unknown(false)
    {}

    RevTree::RevTree(slice raw_tree, sequence seq, uint64_t docOffset)
    :_bodyOffset(docOffset), _sorted(true), _changed(false), _unknown(false)
    {
        decode(raw_tree, seq, docOffset);
    }

    RevTree::~RevTree() {
    }

    void RevTree::decode(forestdb::slice raw_tree, sequence seq, uint64_t docOffset) {
        const RawRevNode *rawNode = (const RawRevNode*)raw_tree.buf;
        unsigned count = countRawNodes(rawNode);
        if (count > UINT16_MAX)
            throw error(error::CorruptRevisionData);
        _bodyOffset = docOffset;
        _nodes.resize(count);
        auto node = _nodes.begin();
        for (; validRawNode(rawNode); rawNode = nextRawNode(rawNode)) {
            nodeFromRawNode(rawNode, &*node);
            if (node->sequence == 0)
                node->sequence = seq;
            node->owner = this;
            node++;
        }
        if ((uint8_t*)rawNode != (uint8_t*)raw_tree.end() - sizeof(uint32_t)) {
            throw error(error::CorruptRevisionData);
        }
    }

    alloc_slice RevTree::encode() {
        sort();

        // Allocate output buffer:
        size_t size = sizeof(uint32_t);  // start with space for trailing 0 size
        for (auto node = _nodes.begin(); node != _nodes.end(); ++node) {
            if (node->body.size > 0 && !(node->isLeaf() || node->isNew())) {
                // Prune body of an already-saved node that's no longer a leaf:
                node->body.buf = NULL;
                node->body.size = 0;
                assert(_bodyOffset > 0);
                node->oldBodyOffset = _bodyOffset;
            }
            size += sizeForRawNode(&*node);
            fprintf(stderr, "Node %p size %lu\n", &*node, size);
        }

        alloc_slice result(size);

        // Write the raw nodes:
        RawRevNode *dst = (RawRevNode*)result.buf;
        for (auto src = _nodes.begin(); src != _nodes.end(); ++src) {
            size_t nodeSize = sizeForRawNode(&*src);
            dst->size = htonl((uint32_t)nodeSize);
            dst->revIDLen = (uint8_t)src->revID.size;
            memcpy(dst->revID, src->revID.buf, src->revID.size);
            dst->parentIndex = htons(src->parentIndex);

            dst->flags = src->flags & kRevNodePublicPersistentFlags;
            if (src->body.size > 0)
                dst->flags |= kRevNodeHasData;
            else if (src->oldBodyOffset > 0)
                dst->flags |= kRevNodeHasBodyOffset;

            void *dstData = offsetby(&dst->revID[0], src->revID.size);
            dstData = offsetby(dstData, PutUVarInt(dstData, src->sequence));
            if (dst->flags & kRevNodeHasData) {
                memcpy(dstData, src->body.buf, src->body.size);
            } else if (dst->flags & kRevNodeHasBodyOffset) {
                /*dstData +=*/ PutUVarInt(dstData, src->oldBodyOffset ?: _bodyOffset);
            }

            dst = (RawRevNode*)offsetby(dst, nodeSize);
        }
        dst->size = htonl(0);   // write trailing 0 size marker
        assert((&dst->size + 1) == result.end());
        return result;
    }

    static size_t sizeForRawNode(const RevNode *node) {
        size_t size = offsetof(RawRevNode, revID) + node->revID.size + SizeOfVarInt(node->sequence);
        if (node->body.size > 0)
            size += node->body.size;
        else if (node->oldBodyOffset > 0)
            size += SizeOfVarInt(node->oldBodyOffset);
        return size;
    }


    static unsigned countRawNodes(const RawRevNode *tree) {
        unsigned count = 0;
        for (const RawRevNode *node = tree; validRawNode(node); node = nextRawNode(node)) {
            ++count;
        }
        return count;
    }


    static void nodeFromRawNode(const RawRevNode *src, RevNode *dst) {
        const void* end = nextRawNode(src);
        dst->revID.buf = (char*)src->revID;
        dst->revID.size = src->revIDLen;
        dst->flags = src->flags & kRevNodePublicPersistentFlags;
        dst->parentIndex = ntohs(src->parentIndex);
        const void *data = offsetby(&src->revID, src->revIDLen);
        ptrdiff_t len = (uint8_t*)end-(uint8_t*)data;
        data = offsetby(data, GetUVarInt((::slice){(void*)data, (size_t)len},
                                         &dst->sequence));
        dst->oldBodyOffset = 0;
        if (src->flags & kRevNodeHasData) {
            dst->body.buf = (char*)data;
            dst->body.size = (char*)end - (char*)data;
        } else {
            dst->body.buf = NULL;
            dst->body.size = 0;
            if (src->flags & kRevNodeHasBodyOffset) {
                slice buf = {(void*)data, (size_t)((uint8_t*)end-(uint8_t*)data)};
                size_t nBytes = GetUVarInt(buf, &dst->oldBodyOffset);
                buf.moveStart(nBytes);
            }
        }
    }

#pragma mark - ACCESSORS:

    const RevNode* RevTree::currentNode() {
        assert(!_unknown);
        sort();
        return &_nodes[0];
    }

    const RevNode* RevTree::get(unsigned index) const {
        assert(!_unknown);
        assert(index < _nodes.size());
        return &_nodes[index];
    }

    const RevNode* RevTree::get(revid revID) const {
        for (auto node = _nodes.begin(); node != _nodes.end(); ++node) {
            if (node->revID.equal(revID))
                return &*node;
        }
        assert(!_unknown);
        return NULL;
    }

    bool RevTree::hasConflict() const {
        if (_nodes.size() < 2) {
            assert(!_unknown);
            return false;
        } else if (_sorted) {
            return _nodes[1].isActive();
        } else {
            unsigned nActive = 0;
            for (auto node = _nodes.begin(); node != _nodes.end(); ++node) {
                if (node->isActive()) {
                    if (++nActive > 1)
                        return true;
                }
            }
            return false;
        }
    }

    std::vector<const RevNode*> RevTree::currentNodes() const {
        assert(!_unknown);
        std::vector<const RevNode*> cur;
        for (auto node = _nodes.begin(); node != _nodes.end(); ++node) {
            if (node->isLeaf())
                cur.push_back(&*node);
        }
        return cur;
    }

    unsigned RevNode::index() const {
        ptrdiff_t index = this - &owner->_nodes[0];
        assert(index >= 0 && index < owner->_nodes.size());
        return (unsigned)index;
    }

    const RevNode* RevNode::parent() const {
        if (parentIndex == RevNode::kNoParent)
            return NULL;
        return owner->get(parentIndex);
    }

    std::vector<const RevNode*> RevNode::history() const {
        std::vector<const RevNode*> h;
        for (const RevNode* node = this; node; node = node->parent())
            h.push_back(node);
        return h;
    }

    bool RevTree::isBodyOfNodeAvailable(const RevNode* node) const {
        return node->body.buf != NULL; // VersionedDocument overrides this
    }

    alloc_slice RevTree::readBodyOfNode(const RevNode* node) const {
        if (node->body.buf != NULL)
            return alloc_slice(node->body);
        return alloc_slice(); // VersionedDocument overrides this
    }


#pragma mark - INSERTION:

    // Lowest-level insert method. Does no sanity checking, always inserts.
    const RevNode* RevTree::_insert(revid unownedRevID,
                                    slice body,
                                    const RevNode *parentNode,
                                    bool deleted)
    {
        assert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.push_back(alloc_slice(unownedRevID));
        revid revID = revid(_insertedData.back());
        _insertedData.push_back(alloc_slice(body));
        body = _insertedData.back();

        RevNode newNode;
        newNode.owner = this;
        newNode.revID = revID;
        newNode.body = body;
        newNode.sequence = 0; // Sequence is unknown till doc is saved
        newNode.oldBodyOffset = 0; // Body position is unknown till doc is saved
        newNode.flags = RevNode::kLeaf | RevNode::kNew;
        if (deleted)
            newNode.flags |= RevNode::kDeleted;

        newNode.parentIndex = RevNode::kNoParent;
        if (parentNode) {
            ptrdiff_t parentIndex = parentNode->index();
            newNode.parentIndex = (uint16_t)parentIndex;
            ((RevNode*)parentNode)->flags &= ~RevNode::kLeaf;
        }

        _nodes.push_back(newNode);

        _changed = true;
        if (_nodes.size() > 1)
            _sorted = false;
        return &_nodes.back();
    }

    const RevNode* RevTree::insert(revid revID, slice data, bool deleted,
                                   const RevNode* parent, bool allowConflict,
                                   int &httpStatus)
    {
        // Make sure the given revID is valid:
        uint32_t newGen = revID.generation();
        if (newGen == 0) {
            httpStatus = 400;
            return NULL;
        }

        if (get(revID)) {
            httpStatus = 200;
            return NULL; // already exists
        }

        // Find the parent node, if a parent ID is given:
        uint32_t parentGen;
        if (parent) {
            if (!allowConflict && !(parent->flags & RevNode::kLeaf)) {
                httpStatus = 409;
                return NULL;
            }
            parentGen = parent->revID.generation();
        } else {
            if (!allowConflict && _nodes.size() > 0) {
                httpStatus = 409;
                return NULL;
            }
            parentGen = 0;
        }

        // Enforce that generation number went up by 1 from the parent:
        if (newGen != parentGen + 1) {
            httpStatus = 400;
            return NULL;
        }
        
        // Finally, insert:
        httpStatus = deleted ? 200 : 201;
        return _insert(revID, data, parent, deleted);
    }

    const RevNode* RevTree::insert(revid revID, slice body, bool deleted,
                                   revid parentRevID, bool allowConflict,
                                   int &httpStatus)
    {
        const RevNode* parent = NULL;
        if (parentRevID.buf) {
            parent = get(parentRevID);
            if (!parent) {
                httpStatus = 404;
                return NULL; // parent doesn't exist
            }
        }
        return insert(revID, body, deleted, parent, allowConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revid> history, slice data, bool deleted) {
        assert(history.size() > 0);
        // Find the common ancestor, if any. Along the way, preflight revision IDs:
        int i;
        unsigned lastGen = 0;
        const RevNode* parent = NULL;
        size_t historyCount = history.size();
        for (i = 0; i < historyCount; i++) {
            unsigned gen = history[i].generation();
            if (lastGen > 0 && gen != lastGen - 1)
                return -1; // generation numbers not in sequence
            lastGen = gen;

            parent = get(history[i]);
            if (parent)
                break;
        }
        int commonAncestorIndex = i;

        // Insert all the new revisions in chronological order:
        while (--i >= 0) {
            parent = _insert(history[i],
                             (i==0 ? data : slice()),
                             parent,
                             (i==0 && deleted));
        }
        return commonAncestorIndex;
    }

    unsigned RevTree::prune(unsigned maxDepth) {
        if (maxDepth == 0 || _nodes.size() <= maxDepth)
            return 0;

        // First find all the leaves, and walk from each one down to its root:
        int numPruned = 0;
        RevNode* node = &_nodes[0];
        for (unsigned i=0; i<_nodes.size(); i++,node++) {
            if (node->isLeaf()) {
                // Starting from a leaf node, trace its ancestry to find its depth:
                unsigned depth = 0;
                for (RevNode* anc = node; anc; anc = (RevNode*)anc->parent()) {
                    if (++depth > maxDepth) {
                        // Mark nodes that are too far away:
                        anc->revID.size = 0;
                        numPruned++;
                    }
                }
            } else if (_sorted) {
                break;
            }
        }
        if (numPruned > 0)
            compact();
        return numPruned;
    }

    std::vector<revid> RevTree::purge(std::vector<revid>revIDs) {
        std::vector<revid> purged;
        bool madeProgress, foundNonLeaf;
        do {
            madeProgress = foundNonLeaf = false;
            for (auto revID = revIDs.begin(); revID != revIDs.end(); ++revID) {
                RevNode* node = (RevNode*)get(*revID);
                if (node) {
                    if (node->isLeaf()) {
                        purged.push_back(*revID);
                        madeProgress = true;
                        node->revID.size = 0; // mark for purge
                        revID->size = 0;
                        revID->buf = NULL; // mark as used
                        if (node->parentIndex != RevNode::kNoParent)
                            _nodes[node->parentIndex].flags |= RevNode::kLeaf;
                    } else {
                        foundNonLeaf = true;
                    }
                }
            }
        } while (madeProgress && foundNonLeaf);
        if (purged.size() > 0)
            compact();
        return purged;
    }

    void RevTree::compact() {
        // Create a mapping from current to new node indexes (after removing pruned/purged nodes)
        uint16_t map[_nodes.size()];
        unsigned i = 0, j = 0;
        for (auto node = _nodes.begin(); node != _nodes.end(); ++node, ++i) {
            if (node->revID.size > 0)
                map[i] = (uint16_t)(j++);
            else
                map[i] = RevNode::kNoParent;
        }

        // Finally, slide the surviving nodes down and renumber their parent indexes:
        RevNode* node = &_nodes[0];
        RevNode* dst = node;
        for (i=0; i<_nodes.size(); i++,node++) {
            if (node->revID.size > 0) {
                node->parentIndex = map[node->parentIndex];
                if (dst != node)
                    *dst = *node;
                dst++;
            }
        }
        _nodes.resize(dst - &_nodes[0]);
        _changed = true;
    }

    // Sort comparison function for an arry of RevNodes.
    bool RevNode::operator<(const RevNode& rev2) const
    {
        // Leaf nodes go first.
        int delta = rev2.isLeaf() - this->isLeaf();
        if (delta)
            return delta < 0;
        // Else non-deleted nodes go first.
        delta = this->isDeleted() - rev2.isDeleted();
        if (delta)
            return delta < 0;
        // Otherwise compare rev IDs, with higher rev ID going first:
        return rev2.revID < this->revID;
    }

    void RevTree::sort() {
        if (_sorted)
            return;

        // oldParents maps node index to the original parentIndex, before the sort.
        // At the same time we change parentIndex[i] to i, so we can track what the sort did.
        uint16_t oldParents[_nodes.size()];
        for (uint16_t i = 0; i < _nodes.size(); ++i) {
            oldParents[i] = _nodes[i].parentIndex;
            _nodes[i].parentIndex = i;
        }

        std::sort(_nodes.begin(), _nodes.end());

        // oldToNew maps old array indexes to new (sorted) ones.
        uint16_t oldToNew[_nodes.size()];
        for (uint16_t i = 0; i < _nodes.size(); ++i) {
            uint16_t oldIndex = _nodes[i].parentIndex;
            oldToNew[oldIndex] = i;
        }

        // Now fix up the parentIndex values by running them through oldToNew:
        for (unsigned i = 0; i < _nodes.size(); ++i) {
            uint16_t oldIndex = _nodes[i].parentIndex;
            uint16_t parent = oldParents[oldIndex];
            if (parent != RevNode::kNoParent)
                parent = oldToNew[parent];
                _nodes[i].parentIndex = parent;
                }
        _sorted = true;

        fprintf(stderr, "Sorted revs:\n");
        for (auto node=_nodes.begin(); node != _nodes.end(); ++node) {
            fprintf(stderr, "    %s\n", ((std::string)node->revID).c_str());
        }
    }
        
}
