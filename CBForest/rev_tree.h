//
//  rev_tree.h
//  couchstore
//
//  Created by Jens Alfke on 11/23/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#ifndef COUCHSTORE_REV_TREE_H
#define COUCHSTORE_REV_TREE_H
#import "forestdb.h"
#include <stdbool.h>
#include <stdlib.h>


// If defined, previous revision bodies will be stored as file offsets to their obsolete docs
#undef REVTREE_USES_FILE_OFFSETS


typedef struct {
    void* buf;
    size_t size;
} sized_buf;


/** RevNode.parentIndex value denoting "no parent". */
#define kRevNodeParentIndexNone UINT16_MAX


enum {
    kRevTreeErrDocNotFound = 1,
    kRevTreeErrAllocFailed
};


enum {
    kRevNodeIsDeleted = 0x01,    /**< Is this revision a deletion/tombstone? */
    kRevNodeIsLeaf    = 0x02,    /**< Is this revision a leaf (no children?) */
};
typedef uint8_t RevNodeFlags;


/** In-memory representation of a single revision's metadata. */
typedef struct RevNode {
    sized_buf   revID;          /**< Revision ID */
    sized_buf   data;           /**< Revision body (JSON), or empty if not stored in this tree */
#ifdef REVTREE_USES_FILE_OFFSETS
    off_t       body_offset;    /**< File offset of Doc containing revision body, or else 0 */
#endif
    uint64_t    sequence;       /**< DB sequence number that this revision has/had */
    uint16_t    parentIndex;    /**< Index in tree's node[] array of parent revision, if any */
    RevNodeFlags flags;         /**< Leaf/deleted flags */
} RevNode;


/** In-memory representation of a revision tree. Basically just a dynamic array of RevNodes.
    The node at index 0 is always the current default/winning revision. */
typedef struct RevTree RevTree;


RevTree* RevTreeNew(unsigned capacity);

static inline void RevTreeFree(RevTree *tree) {free(tree);}


/** Converts a serialized RevTree into in-memory form.
 *  The RevTree contains pointers into the serialized data, so the memory pointed to by
 *  raw_tree must remain valid until after the RevTree* is freed. */
RevTree* RevTreeDecode(sized_buf raw_tree, unsigned extraCapacity,
                       uint64_t sequence, uint64_t body_offset);

/** Serializes a RevTree. Caller is responsible for freeing the returned block.
    The document's content_meta flags should include COUCH_DOC_IS_REVISIONED. */
sized_buf RevTreeEncode(RevTree *tree);

/** Returns the number of nodes in a tree. */
unsigned RevTreeGetCount(RevTree *tree);

/** Returns the current/default/winning node. */
const RevNode* RevTreeGetCurrentNode(RevTree *tree);

/** Gets a node from the tree by its index. Returns NULL if index is out of range. */
const RevNode* RevTreeGetNode(RevTree *tree, unsigned index);

/** Finds a node in a tree given its rev ID. */
const RevNode* RevTreeFindNode(RevTree *tree, sized_buf revID);

/** Gets a node in a rev tree by index, without parsing it first.
 *  This is more efficient if you only need to look up one node.
 *  Returns NULL if index is out of range. */
bool RevTreeRawGetNode(sized_buf raw_tree, unsigned index, RevNode *outNode);

/** Finds a node in a rev tree without parsing it first.
 *  This is more efficient if you only need to look up one node. */
bool RevTreeRawFindNode(sized_buf raw_tree, sized_buf revID, RevNode *outNode);


/** Reserves room for up to 'extraCapacity' insertions.
    May reallocate the tree, invalidating all existing RevNode pointers. */
bool RevTreeReserveCapacity(RevTree **pTree, unsigned extraCapacity);

/** Adds a revision to a tree. The tree's capacity MUST be greater than its count.
 *  The memory pointed to by revID and data MUST remain valid until after the tree is freed. */
bool RevTreeInsert(RevTree **treePtr,
                   sized_buf revID,
                   sized_buf data,
                   sized_buf parentRevID,
                   bool deleted,
                   off_t currentBodyOffset);

/** Sorts the nodes of a tree so that the current node(s) come first.
    Nodes are normally sorted already, but RevTreeInsert will leave them unsorted.
    Note that sorting will invalidate any pre-existing RevNode pointers!
    RevTreeSort is called automatically by RevTreeEncode. */
void RevTreeSort(RevTree *tree);

/** Returns true if the tree has a conflict (multiple nondeleted leaf revisions.) */
bool RevTreeHasConflict(RevTree *tree);

#ifdef REVTREE_USES_FILE_OFFSETS
/** Removes all body file offsets in an encoded tree; should be called as part of a document
    mutator during compaction, since compaction invalidates all existing file offsets.
    Returns true if any changes were made. */
bool RevTreeRawClearBodyOffsets(sized_buf *raw_tree);
#endif

/** Reads the data of a node. If the data is not inline but the node has a body_offset value,
 *  this will read the old document at that body_offset from the database and return the inline
 *  value found there.
 *  @param node The node to get the data of.
 *  @param db  The database the node was read from.
 *  @param data  On return, will be filled in with a pointer to the data.
 *  @param freeData  On return, will be set to true if the caller needs to call free()
 *              on data.buf after finishing with it, else false.
 *  @return COUCHSTORE_SUCCESS or COUCHSTORE_ERROR_DOC_NOT_FOUND, or an I/O error.
 */
bool RevTreeReadNodeData(const RevNode *node,
                         fdb_handle *db,
                         sized_buf *data,
                         bool *freeData);


/** Parses a revision ID into its generation-number prefix and digest suffix.
    The generation number will be nonzero. The digest will be non-empty.
    Returns false if the ID is not of the right form. */
bool RevIDParse(sized_buf rev, unsigned *generation, sized_buf *digest);

/** Parses a compacted revision ID. */
bool RevIDParseCompacted(sized_buf rev, unsigned *generation, sized_buf *digest);

/** Compacts a compressed revision ID. The compact form is written to dstrev->buf (which must
    be large enough to contain it), and dstrev->size is updated.
    Returns false if the input ID is not parseable. */
bool RevIDCompact(sized_buf srcrev, sized_buf *dstrev);

/** Returns the expanded size of a compressed revision ID: the number of bytes of the expanded form.
    If an uncompressed revision ID is given, 0 is returned to signal that expansion isn't needed. */
size_t RevIDExpandedSize(sized_buf rev);

/** Expands a compressed revision ID. The expanded form is written to expanded_rev->buf (which must
    be large enough to contain it), and expanded_rev->size is updated.
    If the revision ID is not compressed, it's copied as-is to expanded_rev. */
void RevIDExpand(sized_buf rev, sized_buf* expanded_rev);


#endif
