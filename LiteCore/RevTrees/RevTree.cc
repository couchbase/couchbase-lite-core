//
//  RevTree.cc
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

#include "RevTree.hh"
#include "RawRevTree.hh"
#include "Error.hh"
#include <algorithm>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if DEBUG
#include <ostream>
#include <sstream>
#endif


namespace litecore {
    using namespace fleece;

    RevTree::RevTree(slice raw_tree, sequence seq)
    :_revs(RawRevision::decodeTree(raw_tree, this, seq))
    {
    }

    void RevTree::decode(litecore::slice raw_tree, sequence seq) {
        _revs = RawRevision::decodeTree(raw_tree, this, seq);
    }

    alloc_slice RevTree::encode() {
        sort();
        return RawRevision::encodeTree(_revs);
    }

#if DEBUG
    void Rev::dump(std::ostream& out) {
        out << "(" << sequence << ") " << (std::string)revID.expanded() << "  ";
        if (isLeaf())
            out << " leaf";
        if (isDeleted())
            out << " del";
        if (hasAttachments())
            out << " attachments";
        if (isNew())
            out << " (new)";
    }
#endif

#pragma mark - ACCESSORS:

    const Rev* RevTree::currentRevision() {
        Assert(!_unknown);
        sort();
        return _revs.size() == 0 ? nullptr : &_revs[0];
    }

    const Rev* RevTree::get(unsigned index) const {
        Assert(!_unknown);
        Assert(index < _revs.size());
        return &_revs[index];
    }

    const Rev* RevTree::get(revid revID) const {
        for (auto &rev : _revs) {
            if (rev.revID == revID)
                return &rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    const Rev* RevTree::getBySequence(sequence seq) const {
        for (auto &rev : _revs) {
            if (rev.sequence == seq)
                return &rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    bool RevTree::hasConflict() const {
        if (_revs.size() < 2) {
            Assert(!_unknown);
            return false;
        } else if (_sorted) {
            return _revs[1].isActive();
        } else {
            unsigned nActive = 0;
            for (auto &rev : _revs) {
                if (rev.isActive()) {
                    if (++nActive > 1)
                        return true;
                }
            }
            return false;
        }
    }

    std::vector<const Rev*> RevTree::currentRevisions() const {
        Assert(!_unknown);
        std::vector<const Rev*> cur;
        for (auto &rev : _revs) {
            if (rev.isLeaf())
                cur.push_back(&rev);
        }
        return cur;
    }

    unsigned Rev::index() const {
        ptrdiff_t index = this - &owner->_revs[0];
        Assert(index >= 0 && index < owner->_revs.size());
        return (unsigned)index;
    }

    const Rev* Rev::parent() const {
        if (_parentIndex == Rev::kNoParent)
            return nullptr;
        return owner->get(_parentIndex);
    }

    const Rev* Rev::next() const {
        auto i = index() + 1;
        return i < owner->size() ? owner->get(i) : nullptr;
    }

    std::vector<const Rev*> Rev::history() const {
        std::vector<const Rev*> h;
        for (const Rev* rev = this; rev; rev = rev->parent())
            h.push_back(rev);
        return h;
    }

    bool RevTree::isBodyOfRevisionAvailable(const Rev* rev) const {
        return rev->_body.buf != nullptr; // VersionedDocument overrides this
    }

    alloc_slice RevTree::readBodyOfRevision(const Rev* rev) const {
        if (rev->_body.buf != nullptr)
            return alloc_slice(rev->_body);
        return alloc_slice(); // VersionedDocument overrides this
    }

    bool RevTree::confirmLeaf(Rev* testRev) {
        int index = testRev->index();
        for (auto &rev : _revs)
            if (rev._parentIndex == index)
                return false;
        testRev->addFlag(Rev::kLeaf);
        return true;
    }
    

#pragma mark - INSERTION:

    // Lowest-level insert method. Does no sanity checking, always inserts.
    const Rev* RevTree::_insert(revid unownedRevID,
                                slice body,
                                const Rev *parentRev,
                                Rev::Flags revFlags)
    {
        Assert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.emplace_back(unownedRevID);
        revid revID = revid(_insertedData.back());
        _insertedData.emplace_back(body);
        body = _insertedData.back();

        Rev newRev;
        newRev.owner = this;
        newRev.revID = revID;
        newRev._body = body;
        newRev.sequence = 0; // Sequence is unknown till record is saved
        newRev.flags = (Rev::Flags)(Rev::kLeaf | Rev::kNew |
                            (revFlags & (Rev::kDeleted | Rev::kHasAttachments | Rev::kKeepBody)));

        newRev._parentIndex = Rev::kNoParent;
        if (parentRev) {
            ptrdiff_t parentIndex = parentRev->index();
            newRev._parentIndex = (uint16_t)parentIndex;
            ((Rev*)parentRev)->clearFlag(Rev::kLeaf);
        }

        _revs.push_back(newRev);

        _changed = true;
        if (_revs.size() > 1)
            _sorted = false;
        return &_revs.back();
    }

    const Rev* RevTree::insert(revid revID, slice data, Rev::Flags revFlags,
                                   const Rev* parent, bool allowConflict,
                                   int &httpStatus)
    {
        // Make sure the given revID is valid:
        uint32_t newGen = revID.generation();
        if (newGen == 0) {
            httpStatus = 400;
            return nullptr;
        }

        if (get(revID)) {
            httpStatus = 200;
            return nullptr; // already exists
        }

        // Find the parent rev, if a parent ID is given:
        uint32_t parentGen;
        if (parent) {
            if (!allowConflict && !parent->isLeaf()) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = parent->revID.generation();
        } else {
            if (!allowConflict && _revs.size() > 0) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = 0;
        }

        // Enforce that generation number went up by 1 from the parent:
        if (newGen != parentGen + 1) {
            httpStatus = 400;
            return nullptr;
        }
        
        // Finally, insert:
        httpStatus = (revFlags & Rev::kDeleted) ? 200 : 201;
        return _insert(revID, data, parent, revFlags);
    }

    const Rev* RevTree::insert(revid revID, slice body, Rev::Flags revFlags,
                                   revid parentRevID, bool allowConflict,
                                   int &httpStatus)
    {
        const Rev* parent = nullptr;
        if (parentRevID.buf) {
            parent = get(parentRevID);
            if (!parent) {
                httpStatus = 404;
                return nullptr; // parent doesn't exist
            }
        }
        return insert(revID, body, revFlags, parent, allowConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revidBuffer> history, slice data,
                               Rev::Flags revFlags) {
        Assert(history.size() > 0);
        // Find the common ancestor, if any. Along the way, preflight revision IDs:
        int i;
        unsigned lastGen = 0;
        const Rev* parent = nullptr;
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

        if (i > 0) {
            // Insert all the new revisions in chronological order:
            while (--i > 0)
                parent = _insert(history[i], slice(), parent, (Rev::Flags)0);
            _insert(history[0], data, parent, revFlags);
        }
        return commonAncestorIndex;
    }

    void RevTree::removeBody(const Rev* rev) {
        if (rev->flags & Rev::kKeepBody) {
            const_cast<Rev*>(rev)->removeBody();
            _changed = true;
        }
    }

    // Remove bodies of already-saved revs that are no longer leaves:
    void RevTree::removeNonLeafBodies() {
        for (auto &rev : _revs) {
            if (rev._body.size > 0 && !(rev.flags & (Rev::kLeaf | Rev::kNew | Rev::kKeepBody)))
                rev._body = nullslice;
        }
    }

    unsigned RevTree::prune(unsigned maxDepth) {
        if (maxDepth == 0 || _revs.size() <= maxDepth)
            return 0;

        // First find all the leaves, and walk from each one down to its root:
        int numPruned = 0;
        Rev* rev = &_revs[0];
        for (unsigned i=0; i<_revs.size(); i++,rev++) {
            if (rev->isLeaf()) {
                // Starting from a leaf rev, trace its ancestry to find its depth:
                unsigned depth = 0;
                for (Rev* anc = rev; anc; anc = (Rev*)anc->parent()) {
                    if (++depth > maxDepth) {
                        // Mark revs that are too far away:
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

    int RevTree::purge(revid leafID) {
        int nPurged = 0;
        Rev* rev = (Rev*)get(leafID);
        if (!rev || !rev->isLeaf())
            return 0;
        do {
            nPurged++;
            rev->revID.size = 0;                    // mark for purge
            const Rev* parent = (Rev*)rev->parent();
            rev->_parentIndex = Rev::kNoParent; // unlink from parent
            rev = (Rev*)parent;
        } while (rev && confirmLeaf(rev));
        compact();
        return nPurged;
    }

    int RevTree::purgeAll() {
        int result = (int)_revs.size();
        _revs.resize(0);
        _changed = true;
        return result;
    }

    void RevTree::compact() {
        // Create a mapping from current to new rev indexes (after removing pruned/purged revs)
		std::vector<uint16_t> map(_revs.size());
        unsigned i = 0, j = 0;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev, ++i) {
            if (rev->revID.size > 0)
                map[i] = (uint16_t)(j++);
            else
                map[i] = Rev::kNoParent;
        }

        // Finally, slide the surviving revs down and renumber their parent indexes:
        Rev* rev = &_revs[0];
        Rev* dst = rev;
        for (i=0; i<_revs.size(); i++,rev++) {
            if (rev->revID.size > 0) {
                if (rev->_parentIndex != Rev::kNoParent)
                    rev->_parentIndex = map[rev->_parentIndex];
                if (dst != rev)
                    *dst = *rev;
                dst++;
            }
        }
        _revs.resize(dst - &_revs[0]);
        _changed = true;
    }

    // Sort comparison function for an array of Revisions. Higher priority comes _first_.
    bool Rev::operator<(const Rev& rev2) const
    {
        // Leaf revs go first.
        int delta = rev2.isLeaf() - this->isLeaf();
        if (delta)
            return delta < 0;
        // Else non-deleted revs go first.
        delta = this->isDeleted() - rev2.isDeleted();
        if (delta)
            return delta < 0;
        // Otherwise compare rev IDs, with higher rev ID going first:
        return rev2.revID < this->revID;
    }

    void RevTree::sort() {
        if (_sorted)
            return;

        // oldParents maps rev index to the original parentIndex, before the sort.
        // At the same time we change parentIndex[i] to i, so we can track what the sort did.
		
        std::vector<uint16_t> oldParents(_revs.size());
        for (uint16_t i = 0; i < _revs.size(); ++i) {
            oldParents[i] = _revs[i]._parentIndex;
            _revs[i]._parentIndex = i;
        }

        std::sort(_revs.begin(), _revs.end());

        // oldToNew maps old array indexes to new (sorted) ones.
		std::vector<uint16_t> oldToNew(_revs.size());
        for (uint16_t i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i]._parentIndex;
            oldToNew[oldIndex] = i;
        }

        // Now fix up the parentIndex values by running them through oldToNew:
        for (unsigned i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i]._parentIndex;
            uint16_t parent = oldParents[oldIndex];
            if (parent != Rev::kNoParent)
                parent = oldToNew[parent];
                _revs[i]._parentIndex = parent;
                }
        _sorted = true;
    }

    void RevTree::saved() {
        for (auto &rev : _revs)
            rev.clearFlag(Rev::kNew);
    }

#if DEBUG
    std::string RevTree::dump() {
        std::stringstream out;
        dump(out);
        return out.str();
    }

    void RevTree::dump(std::ostream& out) {
        int i = 0;
        for (auto &rev : _revs) {
            out << "\t" << (++i) << ": ";
            rev.dump(out);
            out << "\n";
        }
    }
#endif

}
