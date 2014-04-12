//
//  rev_tree.c
//  couchstore
//
//  Created by Jens Alfke on 11/23/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#include "rev_tree.h"
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

#define htonll CFSwapInt64HostToBig
#define ntohll CFSwapInt64BigToHost


// Innards of RevTree struct (in-memory representation)
struct RevTree {
    unsigned    count;          // Number of nodes in tree
    unsigned    capacity;       // Capacity of node[] array
    uint64_t    bodyOffset;     // File offset of body this tree was read from
    uint64_t    bodySequence;
    bool        sorted;         // Are the nodes currently sorted?
    RevNode     node[1];        // Actual dimension is [.capacity]
};


// Private RevNodeFlags bits:
enum {
    kRevNodePublicPersistentFlags = (kRevNodeIsLeaf | kRevNodeIsDeleted),
    kRevNodeHasData = 0x80,    /**< Does this raw node contain JSON data? */
#ifdef REVTREE_USES_FILE_OFFSETS
    kRevNodeHasBodyOffset = 0x40    /**< Does this raw node have a file position (oldBodyOffset)? */
#endif
};

typedef uint64_t raw_bp;

// Layout of revision node in encoded form. Tree is a sequence of these followed by a 32-bit zero.
// Nodes are stored in decending priority, with the current leaf node(s) coming first.
typedef struct {
    uint32_t        size;           // Total size of this tree node
    uint16_t        parentIndex;
    RevNodeFlags    flags;
    uint8_t         revIDLen;
    char            revID[1];       // actual size is [revIDLen]
    // These follow the revID:
    // varint       sequence
    // if HasData flag:
    //    char      data[];       // Contains the revision body (JSON)
    // else:
    //    varint    oldBodyOffset;  // Points to doc that has the body (0 if none)
    //    varint    body_size;
} RawRevNode;

#define nodeIsLeaf(N)    (((N)->flags & kRevNodeIsLeaf) != 0)
#define nodeIsDeleted(N) (((N)->flags & kRevNodeIsDeleted) != 0)
#define nodeIsNew(N)     (((N)->flags & kRevNodeIsNew) != 0)
#define nodeIsActive(N)  (nodeIsLeaf(N) && !nodeIsDeleted(N))


static size_t sizeForRawNode(const RevNode *node);
static unsigned countRawNodes(const RawRevNode *tree);
static void nodeFromRawNode(const RawRevNode *src, RevNode *dst);

static inline const RawRevNode* firstRawNode(sized_buf buf)
{
    return (const RawRevNode*)buf.buf;
}

static inline bool validRawNode(const RawRevNode *rawNode)
{
    return ntohl(rawNode->size) > 0;
}

static inline const RawRevNode *nextRawNode(const RawRevNode *node)
{
    return offsetby(node, ntohl(node->size));
}

static int compareNodes(const void *ptr1, const void *ptr2);

static RevNode* parentNode(RevTree* tree, RevNode* node) {
    if (node->parentIndex == kRevNodeParentIndexNone)
        return NULL;
    return &tree->node[node->parentIndex];
}


#pragma mark - PUBLIC API:


RevTree* RevTreeNew(unsigned capacity)
{
    RevTree *tree = malloc(offsetof(RevTree, node) + capacity * sizeof(RevNode));
    if (tree) {
        tree->count = 0;
        tree->capacity = capacity;
        tree->sorted = true;
        tree->bodyOffset = 0;
    }
    return tree;
}


RevTree* RevTreeDecode(sized_buf raw_tree, unsigned extraCapacity,
                       fdb_seqnum_t sequence, uint64_t oldBodyOffset)
{
    const RawRevNode *rawNode = (const RawRevNode*)raw_tree.buf;
    unsigned count = countRawNodes(rawNode);
    unsigned capacity = count + extraCapacity;
    if (capacity > UINT16_MAX)
        return NULL;
    RevTree *tree = RevTreeNew(capacity);
    if (!tree) {
        return NULL;
    }
    RevNode *node = &tree->node[0];
    for (; validRawNode(rawNode); rawNode = nextRawNode(rawNode)) {
        nodeFromRawNode(rawNode, node);
        if (node->sequence == kRevNodeSequenceNone)
            node->sequence = sequence;
        node++;
    }
    if ((void*)rawNode != raw_tree.buf + raw_tree.size - sizeof(uint32_t)) {
        free(tree);
        return NULL;
    }
    tree->count = count;
    tree->bodyOffset = oldBodyOffset;
    tree->bodySequence = sequence;
    return tree;
}


sized_buf RevTreeEncode(RevTree *tree)
{
    RevTreeSort(tree);
    
    // Allocate output buffer:
    sized_buf result = {NULL, 0};
    size_t size = sizeof(uint32_t);  // start with space for trailing 0 size
    RevNode *node = &tree->node[0];
    for (unsigned i = 0; i < tree->count; ++i) {
#ifdef REVTREE_USES_FILE_OFFSETS
        if (node->data.size > 0 && !(nodeIsLeaf(node) || nodeIsNew(node))) {
            // Prune body of an already-saved node that's no longer a leaf:
            node->data.buf = NULL;
            node->data.size = 0;
            node->oldBodyOffset = tree->bodyOffset;
        }
#endif
        size += sizeForRawNode(node++);
    }
    void *buf = malloc(size);
    if (!buf)
        goto exit;

    // Write the raw nodes:
    const RevNode *src = &tree->node[0];
    RawRevNode *dst = buf;
    for (unsigned i = 0; i < tree->count; ++i) {
        size_t nodeSize = sizeForRawNode(src);
        dst->size = htonl((uint32_t)nodeSize);
        dst->revIDLen = (uint8_t)src->revID.size;
        memcpy(dst->revID, src->revID.buf, src->revID.size);
        dst->parentIndex = htons(src->parentIndex);

        dst->flags = src->flags & kRevNodePublicPersistentFlags;
        if (src->data.size > 0)
            dst->flags |= kRevNodeHasData;
#ifdef REVTREE_USES_FILE_OFFSETS
        else if (src->oldBodyOffset > 0)
            dst->flags |= kRevNodeHasBodyOffset;
#endif

        void *dstData = (void*)offsetby(&dst->revID[0], src->revID.size);
        dstData += WriteUVarInt(dstData, src->sequence);
        if (dst->flags & kRevNodeHasData) {
            memcpy(dstData, src->data.buf, src->data.size);
        }
#ifdef REVTREE_USES_FILE_OFFSETS
        else if (dst->flags & kRevNodeHasBodyOffset) {
            dstData += WriteUVarInt(dstData, src->oldBodyOffset ?: tree->bodyOffset);
        }
#endif

        ++src;
        dst = (RawRevNode*)offsetby(dst, nodeSize);
    }
    dst->size = htonl(0);   // write trailing 0 size marker
    assert((char*)(&dst->size + 1) == (char*)buf + size);
    
    result.buf = buf;
    result.size = size;
    buf = NULL;

exit:
    free(buf);
    return result;
}


unsigned RevTreeGetCount(RevTree *tree) {
    return tree->count;
}


const RevNode* RevTreeGetCurrentNode(RevTree *tree) {
    if (tree->sorted) {
        return RevTreeGetNode(tree, 0);
    } else if (tree->count == 0) {
        return NULL;
    } else {
        // Tree is unsorted, so do a linear search for node that sorts first:
        const RevNode *maxNode = &tree->node[0];
        const RevNode *node = maxNode + 1;
        for (unsigned i = 1; i < tree->count; ++i, ++node) {
            if (compareNodes(node, maxNode) < 0) {
                maxNode = node;
            }
        }
        return maxNode;
    }
}


const RevNode* RevTreeGetNode(RevTree *tree, unsigned index)
{
    if (index >= tree->count)
        return NULL;
    return &tree->node[index];
}


const RevNode* RevTreeFindNode(RevTree *tree, sized_buf revID)
{
    const RevNode *node = &tree->node[0];
    for (unsigned i = 0; i < tree->count; ++i, ++node) {
        if (revID.size==node->revID.size && memcmp(revID.buf, node->revID.buf, revID.size) == 0)
            return node;
    }
    return NULL;
}


bool RevTreeRawGetNode(sized_buf raw_tree, unsigned index, RevNode *outNode)
{
    const RawRevNode *rawNode;
    for (rawNode = firstRawNode(raw_tree); validRawNode(rawNode); rawNode = nextRawNode(rawNode)) {
        if (index-- == 0) {
            nodeFromRawNode(rawNode, outNode);
            return true;
        }
    }
    return false;
}


bool RevTreeRawFindNode(sized_buf raw_tree, sized_buf revID, RevNode *outNode)
{
    const RawRevNode *rawNode;
    for (rawNode = firstRawNode(raw_tree); validRawNode(rawNode); rawNode = nextRawNode(rawNode)) {
        if (revID.size == rawNode->revIDLen && memcmp(revID.buf, rawNode->revID, revID.size) == 0) {
            nodeFromRawNode(rawNode, outNode);
            return true;
        }
    }
    return false;
}


bool RevTreeHasConflict(RevTree *tree) {
    if (tree->count < 2) {
        return false;
    } else if (tree->sorted) {
        return nodeIsActive(&tree->node[1]);
    } else {
        unsigned nActive = 0;
        const RevNode *node = &tree->node[0];
        for (unsigned i = 0; i < tree->count; ++i, ++node) {
            if (nodeIsActive(node)) {
                if (++nActive > 1)
                    return true;
            }
        }
        return false;
    }
}


bool RevTreeReserveCapacity(RevTree **pTree, unsigned extraCapacity)
{
    unsigned capacityNeeded = (*pTree)->count + extraCapacity;
    unsigned capacity = (*pTree)->capacity;
    if (capacityNeeded <= capacity)
        return true;
    do {
        capacity *= 2;
    } while (capacity < capacityNeeded);
    
    RevTree *tree = realloc(*pTree, offsetof(RevTree, node) + capacity * sizeof(RevNode));
    if (!tree)
        return false;
    tree->capacity = capacity;
    *pTree = tree;
    return true;
}


static void _revTreeInsert(RevTree *tree,
                           sized_buf revID,
                           sized_buf data,
                           const RevNode *parentNode,
                           bool deleted)
{
    assert(tree->count < tree->capacity);
    RevNode *newNode = &tree->node[tree->count++];
    newNode->revID = revID;
    newNode->data = data;
    newNode->sequence = kRevNodeSequenceNone; // Sequence is unknown till doc is saved
#ifdef REVTREE_USES_FILE_OFFSETS
    newNode->oldBodyOffset = 0; // Body position is unknown till doc is saved
#endif
    newNode->flags = kRevNodeIsLeaf | kRevNodeIsNew;
    if (deleted)
        newNode->flags |= kRevNodeIsDeleted;

    newNode->parentIndex = kRevNodeParentIndexNone;
    if (parentNode) {
        ptrdiff_t parentIndex = parentNode - &tree->node[0];
        assert(parentIndex >= 0 && parentIndex < tree->count - 1);
        newNode->parentIndex = (uint16_t)parentIndex;
        ((RevNode*)parentNode)->flags &= ~kRevNodeIsLeaf;
    }

    if (tree->count > 1)
        tree->sorted = false;
}


bool RevTreeInsert(RevTree **treeP,
                   sized_buf revID,
                   sized_buf data,
                   sized_buf parentRevID,
                   bool deleted)
{
    if (!RevTreeReserveCapacity(treeP, 1))
        return false;

    // Make sure the given revID is valid but doesn't exist yet:
    uint32_t newSeq;
    if (!RevIDParseCompacted(revID, &newSeq, NULL))
        return false;
    if (RevTreeFindNode(*treeP, revID))
        return false;

    // Find the parent node, if a parent ID is given:
    const RevNode* parent = NULL;
    uint32_t parentSeq = 0;
    if (parentRevID.buf) {
        parent = RevTreeFindNode(*treeP, parentRevID);
        if (!parent || !RevIDParseCompacted(parentRevID, &parentSeq, NULL))
            return false;
    }
    
    // Enforce that sequence number went up by 1 from the parent:
    if (newSeq != parentSeq + 1)
        return false;

    // Finally, insert:
    _revTreeInsert(*treeP, revID, data, parent, deleted);
    return true;
}


void RevTreePrune(RevTree* tree, unsigned maxDepth) {
    if (maxDepth == 0 || tree->count <= maxDepth)
        return;

    // First find all the leaves, and walk from each one down to its root:
    bool pruned = false;
    RevNode* node = &tree->node[0];
    for (unsigned i=0; i<tree->count; i++,node++) {
        if (nodeIsLeaf(node)) {
            // Starting from a leaf node, trace its ancestry to find its depth:
            unsigned depth = 0;
            for (RevNode* anc = node; anc; anc = parentNode(tree, anc)) {
                if (++depth > maxDepth) {
                    // Mark nodes that are too far away:
                    anc->revID.size = 0;
                    pruned = true;
                }
            }
        } else if (tree->sorted) {
            break;
        }
    }
    if (!pruned)
        return;

    // Next, create a mapping from current to new node indexes (after removing pruned nodes)
    uint16_t* map = malloc(tree->count * sizeof(uint16_t));
    node = &tree->node[0];
    for (unsigned i=0, j=0; i<tree->count; i++,node++) {
        if (node->revID.size > 0)
            map[i] = j++;
        else
            map[i] = kRevNodeParentIndexNone;
    }

    // Finally, slide the surviving nodes down and renumber their parent indexes:
    node = &tree->node[0];
    RevNode* dst = node;
    for (unsigned i=0; i<tree->count; i++,node++) {
        if (node->revID.size > 0) {
            node->parentIndex = map[node->parentIndex];
            if (dst != node)
                *dst = *node;
            dst++;
        }
    }
    tree->count = (unsigned)(dst - &tree->node[0]);
    free(map);
}


#ifdef REVTREE_USES_FILE_OFFSETS
static void spliceOut(sized_buf *buf, void *start, size_t len)
{
    ptrdiff_t offset = (char*)start - (char*)buf->buf;
    ptrdiff_t remaining = buf->size - offset;
    assert(offset > 0 && remaining >= 0 && remaining < (ptrdiff_t)buf->size);
    memcpy(start, offsetby(start, len), remaining);
    buf->size -= len;
}


bool RevTreeRawClearBodyOffsets(sized_buf *raw_tree)
{
    bool changed = false;
    RawRevNode *src = (RawRevNode*)firstRawNode(*raw_tree);
    while(validRawNode(src)) {
        RawRevNode *next = (RawRevNode*)nextRawNode(src);
        if (src->flags & kRevNodeHasBodyOffset) {
            src->flags &= ~kRevNodeHasBodyOffset;
            next = offsetby(next, -sizeof(raw_bp));
            spliceOut(raw_tree, next, sizeof(raw_bp));
            changed = true;
        }
        src = next;
    }
    return changed;
}
#endif

#pragma mark - RAW-NODE OPERATIONS:


static size_t sizeForRawNode(const RevNode *node)
{
    size_t size = offsetof(RawRevNode, revID) + node->revID.size + SizeOfVarInt(node->sequence);
    if (node->data.size > 0)
        size += node->data.size;
#ifdef REVTREE_USES_FILE_OFFSETS
    else if (node->oldBodyOffset > 0)
        size += SizeOfVarInt(node->oldBodyOffset);
#endif
    return size;
}


static unsigned countRawNodes(const RawRevNode *tree)
{
    unsigned count = 0;
    for (const RawRevNode *node = tree; validRawNode(node); node = nextRawNode(node)) {
        ++count;
    }
    return count;
}


static void nodeFromRawNode(const RawRevNode *src, RevNode *dst)
{
    const void* end = nextRawNode(src);
    dst->revID.buf = (char*)src->revID;
    dst->revID.size = src->revIDLen;
    dst->flags = src->flags & kRevNodePublicPersistentFlags;
    dst->parentIndex = ntohs(src->parentIndex);
    const void *data = offsetby(&src->revID, src->revIDLen);
    data += ReadUVarInt((sized_buf){(void*)data, end-data}, &dst->sequence);
#ifdef REVTREE_USES_FILE_OFFSETS
    dst->oldBodyOffset = 0;
#endif
    if (src->flags & kRevNodeHasData) {
        dst->data.buf = (char*)data;
        dst->data.size = (char*)end - (char*)data;
    } else {
        dst->data.buf = NULL;
        dst->data.size = 0;
#ifdef REVTREE_USES_FILE_OFFSETS
        if (src->flags & kRevNodeHasBodyOffset) {
            sized_buf buf = {(void*)data, end-data};
            size_t nBytes = ReadUVarInt(buf, &dst->oldBodyOffset);
            buf.buf += nBytes;
            buf.size -= nBytes;
        }
#endif
    }
}


#pragma mark - REVISION IDS:


// Parses bytes from str to end as an ASCII number. Returns 0 if non-digit found.
static uint32_t parseDigits(const char *str, const char *end)
{
    uint32_t result = 0;
    for (; str < end; ++str) {
        if (!isdigit(*str))
            return 0;
        result = 10*result + (*str - '0');
    }
    return result;
}


bool RevIDParse(sized_buf rev, unsigned *generation, sized_buf *digest) {
    const char *dash = memchr(rev.buf, '-', rev.size);
    if (dash == NULL || dash == rev.buf) {
        return false;
    }
    ssize_t dashPos = dash - (const char*)rev.buf;
    if (dashPos > 8 || dashPos >= rev.size - 1) {
        return false;
    }
    *generation = parseDigits(rev.buf, dash);
    if (*generation == 0) {
        return false;
    }
    if (digest) {
        digest->buf = (char*)dash + 1;
        digest->size = rev.buf + rev.size - digest->buf;
    }
    return true;
}


bool RevIDParseCompacted(sized_buf rev, unsigned *generation, sized_buf *digest) {
    unsigned gen = ((uint8_t*)rev.buf)[0];
    if (isdigit(gen))
        return RevIDParse(rev, generation, digest);
    if (gen > '9')
        gen -= 10;
    *generation = gen;
    if (digest)
        *digest = (sized_buf){rev.buf + 1, rev.size - 1};
    return true;
}


static void copyBuf(sized_buf srcrev, sized_buf *dstrev) {
    dstrev->size = srcrev.size;
    memcpy(dstrev->buf, srcrev.buf, srcrev.size);
}


bool RevIDCompact(sized_buf srcrev, sized_buf *dstrev) {
    unsigned generation;
    sized_buf digest;
    if (!RevIDParse(srcrev, &generation, &digest))
        return false;
    else if (generation > 245 || (digest.size & 1)) {
        copyBuf(srcrev, dstrev);
        return true;
    }
    const char* src = digest.buf;
    for (unsigned i=0; i<digest.size; i++) {
        if (!isxdigit(src[i])) {
            copyBuf(srcrev, dstrev);
            return true;
        }
    }

    uint8_t encodedGen = (uint8_t)generation;
    if (generation >= '0')
        encodedGen += 10; // skip digit range
    char* buf = dstrev->buf, *dst = buf;
    *dst++ = encodedGen;
    for (unsigned i=0; i<digest.size; i+=2)
        *dst++ = 16*digittoint(src[i]) + digittoint(src[i+1]);
    dstrev->size = dst - buf;
    return true;
}


size_t RevIDExpandedSize(sized_buf rev) {
    unsigned generation = ((const uint8_t*)rev.buf)[0];
    if (isdigit(generation))
        return 0; // uncompressed
    if (generation > '9')
        generation -= 10;
    return 2 + (generation >= 10) + (generation >= 100) + 2*(rev.size-1);
}


void RevIDExpand(sized_buf rev, sized_buf* expanded_rev) {
    const uint8_t* src = rev.buf;
    if (isdigit(src[0])) {
        expanded_rev->size = rev.size;
        memcpy(expanded_rev->buf, rev.buf, rev.size);
        return;
    }
    unsigned generation = src[0];
    if (generation > '9')
        generation -= 10;
    char *buf = expanded_rev->buf, *dst = buf;
    dst += sprintf(dst, "%u-", generation);
    for (unsigned i=1; i<rev.size; i++)
        dst += sprintf(dst, "%02x", src[i]);
    expanded_rev->size = dst - buf;
}


#pragma mark - SORTING:


void RevTreeSort(RevTree *tree)
{
    if (tree->sorted)
        return;

    // oldParents maps node index to the original parentIndex, before the sort.
    // At the same time we change parentIndex[i] to i, so we can track what the sort did.
    uint16_t oldParents[tree->count];
    for (uint16_t i = 0; i < tree->count; ++i) {
        oldParents[i] = tree->node[i].parentIndex;
        tree->node[i].parentIndex = i;
    }
    
    qsort(&tree->node[0], tree->count, sizeof(RevNode), compareNodes);

    // oldToNew maps old array indexes to new (sorted) ones.
    uint16_t oldToNew[tree->count];
    for (uint16_t i = 0; i < tree->count; ++i) {
        uint16_t oldIndex = tree->node[i].parentIndex;
        oldToNew[oldIndex] = i;
    }

    // Now fix up the parentIndex values by running them through oldToNew:
    for (unsigned i = 0; i < tree->count; ++i) {
        uint16_t oldIndex = tree->node[i].parentIndex;
        uint16_t parent = oldParents[oldIndex];
        if (parent != kRevNodeParentIndexNone)
            parent = oldToNew[parent];
        tree->node[i].parentIndex = parent;
    }
    tree->sorted = true;
}


static int buf_cmp(sized_buf a, sized_buf b) {
    size_t minSize = a.size < b.size ? a.size : b.size;
    int result = memcmp(a.buf, b.buf, minSize);
    if (result == 0) {
        if (a.size < b.size)
            result = -1;
        else if (a.size > b.size)
            result = 1;
    }
    return result;
}


/*  A proper revision ID consists of a generation number, a hyphen, and an arbitrary suffix.
    Compare the generation numbers numerically, and then the suffixes lexicographically.
    If either string isn't a proper rev ID, fall back to lexicographic comparison. */
static int compareRevIDs(sized_buf rev1, sized_buf rev2)
{
    uint32_t gen1, gen2;
    sized_buf digest1, digest2;
    if (!RevIDParse(rev1, &gen1, &digest1) || !RevIDParse(rev2, &gen2, &digest2)) {
        // Improper rev IDs; just compare as plain text:
        return buf_cmp(rev1, rev2);
    }
    // Compare generation numbers; if they match, compare suffixes:
    if (gen1 > gen2)
        return 1;
    else if (gen1 < gen2)
        return -1;
    else
        return buf_cmp(digest1, digest2);
}


// Sort comparison function for an arry of RevNodes.
static int compareNodes(const void *ptr1, const void *ptr2)
{
    const RevNode *n1 = ptr1, *n2 = ptr2;
    // Leaf nodes go first.
    int delta = nodeIsLeaf(n2) - nodeIsLeaf(n1);
    if (delta)
        return delta;
    // Else non-deleted nodes go first.
    delta = nodeIsDeleted(n1) - nodeIsDeleted(n2);
    if (delta)
        return delta;
    // Otherwise compare rev IDs, with higher rev ID going first:
    return compareRevIDs(n2->revID, n1->revID);
}
