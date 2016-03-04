//
//  RevTree.cc
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

#include "RevTree.hh"
#include "varint.hh"
#include <forestdb.h>
#include <algorithm>
#ifdef _MSC_VER
#include <WinSock2.h>
#else
#include <arpa/inet.h>  // for htons, etc.
#endif
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ostream>
#include <sstream>


namespace cbforest {

    // Layout of revision rev in encoded form. Tree is a sequence of these followed by a 32-bit zero.
    // Revs are stored in decending priority, with the current leaf rev(s) coming first.
    class RawRevision {
    public:
        // Private RevisionFlags bits used in encoded form:
        enum : uint8_t {
            kPublicPersistentFlags = (Revision::kLeaf | Revision::kDeleted | Revision::kHasAttachments),
            kHasBodyOffset = 0x40,  /**< Does this raw rev have a file position (oldBodyOffset)? */
            kHasData       = 0x80,  /**< Does this raw rev contain JSON data? */
        };
        
        uint32_t        size;           // Total size of this tree rev
        uint16_t        parentIndex;
        uint8_t         flags;
        uint8_t         revIDLen;
        char            revID[1];       // actual size is [revIDLen]
        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];       // Contains the revision body (JSON)
        // else:
        //    varint    oldBodyOffset;  // Points to doc that has the body (0 if none)
        //    varint    body_size;

        bool isValid() const {
            return size != 0;
        }

        const RawRevision *next() const {
            return (const RawRevision*)offsetby(this, ntohl(size));
        }

        unsigned count() const {
            unsigned count = 0;
            for (const RawRevision *rev = this; rev->isValid(); rev = rev->next())
                ++count;
            return count;
        }
    };


    RevTree::RevTree(slice raw_tree, sequence seq, uint64_t docOffset)
    :_bodyOffset(docOffset)
    {
        decode(raw_tree, seq, docOffset);
    }

    void RevTree::decode(cbforest::slice raw_tree, sequence seq, uint64_t docOffset) {
        const RawRevision *rawRev = (const RawRevision*)raw_tree.buf;
        unsigned count = rawRev->count();
        if (count > UINT16_MAX)
            throw error(error::CorruptRevisionData);
        _bodyOffset = docOffset;
        _revs.resize(count);
        auto rev = _revs.begin();
        for (; rawRev->isValid(); rawRev = rawRev->next()) {
            rev->read(rawRev);
            if (rev->sequence == 0)
                rev->sequence = seq;
            rev->owner = this;
            rev++;
        }
        if ((uint8_t*)rawRev != (uint8_t*)raw_tree.end() - sizeof(uint32_t)) {
            throw error(error::CorruptRevisionData);
        }
    }

    alloc_slice RevTree::encode() {
        sort();

        // Allocate output buffer:
        size_t size = sizeof(uint32_t);  // start with space for trailing 0 size
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->body.size > 0 && !(rev->isLeaf() || rev->isNew())) {
                // Prune body of an already-saved rev that's no longer a leaf:
                rev->body.buf = NULL;
                rev->body.size = 0;
                CBFAssert(_bodyOffset > 0);
                rev->oldBodyOffset = _bodyOffset;
            }
            size += rev->sizeToWrite();
        }

        alloc_slice result(size);

        // Write the raw revs:
        RawRevision *dst = (RawRevision*)result.buf;
        for (auto src = _revs.begin(); src != _revs.end(); ++src) {
            dst = src->write(dst, _bodyOffset);
        }
        dst->size = htonl(0);   // write trailing 0 size marker
        CBFAssert((&dst->size + 1) == result.end());
        return result;
    }

    size_t Revision::sizeToWrite() const {
        size_t size = offsetof(RawRevision, revID) + this->revID.size + SizeOfVarInt(this->sequence);
        if (this->body.size > 0)
            size += this->body.size;
        else if (this->oldBodyOffset > 0)
            size += SizeOfVarInt(this->oldBodyOffset);
        return size;
    }

    RawRevision* Revision::write(RawRevision* dst, uint64_t bodyOffset) const {
        size_t revSize = this->sizeToWrite();
        dst->size = htonl((uint32_t)revSize);
        dst->revIDLen = (uint8_t)this->revID.size;
        memcpy(dst->revID, this->revID.buf, this->revID.size);
        dst->parentIndex = htons(this->parentIndex);

        uint8_t dstFlags = this->flags & RawRevision::kPublicPersistentFlags;
        if (this->body.size > 0)
            dstFlags |= RawRevision::kHasData;
        else if (this->oldBodyOffset > 0)
            dstFlags |= RawRevision::kHasBodyOffset;
        dst->flags = (Revision::Flags)dstFlags;

        void *dstData = offsetby(&dst->revID[0], this->revID.size);
        dstData = offsetby(dstData, PutUVarInt(dstData, this->sequence));
        if (dst->flags & RawRevision::kHasData) {
            memcpy(dstData, this->body.buf, this->body.size);
        } else if (dst->flags & RawRevision::kHasBodyOffset) {
            /*dstData +=*/ PutUVarInt(dstData, this->oldBodyOffset ? this->oldBodyOffset : bodyOffset);
        }

        return (RawRevision*)offsetby(dst, revSize);
    }

    void Revision::read(const RawRevision *src) {
        const void* end = src->next();
        this->revID.buf = (char*)src->revID;
        this->revID.size = src->revIDLen;
        this->flags = (Flags)(src->flags & RawRevision::kPublicPersistentFlags);
        this->parentIndex = ntohs(src->parentIndex);
        const void *data = offsetby(&src->revID, src->revIDLen);
        ptrdiff_t len = (uint8_t*)end-(uint8_t*)data;
        data = offsetby(data, GetUVarInt(slice(data, len), &this->sequence));
        this->oldBodyOffset = 0;
        if (src->flags & RawRevision::kHasData) {
            this->body.buf = (char*)data;
            this->body.size = (char*)end - (char*)data;
        } else {
            this->body.buf = NULL;
            this->body.size = 0;
            if (src->flags & RawRevision::kHasBodyOffset) {
                slice buf = {(void*)data, (size_t)((uint8_t*)end-(uint8_t*)data)};
                size_t nBytes = GetUVarInt(buf, &this->oldBodyOffset);
                buf.moveStart(nBytes);
            }
        }
    }

#if DEBUG
    void Revision::dump(std::ostream& out) {
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

    const Revision* RevTree::currentRevision() {
        CBFAssert(!_unknown);
        sort();
        return _revs.size() == 0 ? NULL : &_revs[0];
    }

    const Revision* RevTree::get(unsigned index) const {
        CBFAssert(!_unknown);
        CBFAssert(index < _revs.size());
        return &_revs[index];
    }

    const Revision* RevTree::get(revid revID) const {
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->revID == revID)
                return &*rev;
        }
        CBFAssert(!_unknown);
        return NULL;
    }

    const Revision* RevTree::getBySequence(sequence seq) const {
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->sequence == seq)
                return &*rev;
        }
        CBFAssert(!_unknown);
        return NULL;
    }

    bool RevTree::hasConflict() const {
        if (_revs.size() < 2) {
            CBFAssert(!_unknown);
            return false;
        } else if (_sorted) {
            return _revs[1].isActive();
        } else {
            unsigned nActive = 0;
            for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
                if (rev->isActive()) {
                    if (++nActive > 1)
                        return true;
                }
            }
            return false;
        }
    }

    std::vector<const Revision*> RevTree::currentRevisions() const {
        CBFAssert(!_unknown);
        std::vector<const Revision*> cur;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->isLeaf())
                cur.push_back(&*rev);
        }
        return cur;
    }

    unsigned Revision::index() const {
        ptrdiff_t index = this - &owner->_revs[0];
        CBFAssert(index >= 0 && index < owner->_revs.size());
        return (unsigned)index;
    }

    const Revision* Revision::parent() const {
        if (parentIndex == Revision::kNoParent)
            return NULL;
        return owner->get(parentIndex);
    }

    const Revision* Revision::next() const {
        auto i = index() + 1;
        return i < owner->size() ? owner->get(i) : NULL;
    }

    std::vector<const Revision*> Revision::history() const {
        std::vector<const Revision*> h;
        for (const Revision* rev = this; rev; rev = rev->parent())
            h.push_back(rev);
        return h;
    }

    bool RevTree::isBodyOfRevisionAvailable(const Revision* rev, uint64_t atOffset) const {
        return rev->body.buf != NULL; // VersionedDocument overrides this
    }

    alloc_slice RevTree::readBodyOfRevision(const Revision* rev, uint64_t atOffset) const {
        if (rev->body.buf != NULL)
            return alloc_slice(rev->body);
        return alloc_slice(); // VersionedDocument overrides this
    }

    bool RevTree::confirmLeaf(Revision* testRev) {
        int index = testRev->index();
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev)
            if (rev->parentIndex == index)
                return false;
        testRev->addFlag(Revision::kLeaf);
        return true;
    }
    

#pragma mark - INSERTION:

    // Lowest-level insert method. Does no sanity checking, always inserts.
    const Revision* RevTree::_insert(revid unownedRevID,
                                     slice body,
                                     const Revision *parentRev,
                                     bool deleted,
                                     bool hasAttachments)
    {
        CBFAssert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.push_back(alloc_slice(unownedRevID));
        revid revID = revid(_insertedData.back());
        _insertedData.push_back(alloc_slice(body));
        body = _insertedData.back();

        Revision newRev;
        newRev.owner = this;
        newRev.revID = revID;
        newRev.body = body;
        newRev.sequence = 0; // Sequence is unknown till doc is saved
        newRev.oldBodyOffset = 0; // Body position is unknown till doc is saved
        newRev.flags = (Revision::Flags)(Revision::kLeaf | Revision::kNew);
        if (deleted)
            newRev.addFlag(Revision::kDeleted);
        if (hasAttachments)
            newRev.addFlag(Revision::kHasAttachments);

        newRev.parentIndex = Revision::kNoParent;
        if (parentRev) {
            ptrdiff_t parentIndex = parentRev->index();
            newRev.parentIndex = (uint16_t)parentIndex;
            ((Revision*)parentRev)->clearFlag(Revision::kLeaf);
        }

        _revs.push_back(newRev);

        _changed = true;
        if (_revs.size() > 1)
            _sorted = false;
        return &_revs.back();
    }

    const Revision* RevTree::insert(revid revID, slice data, bool deleted, bool hasAttachments,
                                   const Revision* parent, bool allowConflict,
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

        // Find the parent rev, if a parent ID is given:
        uint32_t parentGen;
        if (parent) {
            if (!allowConflict && !parent->isLeaf()) {
                httpStatus = 409;
                return NULL;
            }
            parentGen = parent->revID.generation();
        } else {
            if (!allowConflict && _revs.size() > 0) {
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
        return _insert(revID, data, parent, deleted, hasAttachments);
    }

    const Revision* RevTree::insert(revid revID, slice body, bool deleted, bool hasAttachments,
                                   revid parentRevID, bool allowConflict,
                                   int &httpStatus)
    {
        const Revision* parent = NULL;
        if (parentRevID.buf) {
            parent = get(parentRevID);
            if (!parent) {
                httpStatus = 404;
                return NULL; // parent doesn't exist
            }
        }
        return insert(revID, body, deleted, hasAttachments, parent, allowConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revidBuffer> history, slice data,
                               bool deleted, bool hasAttachments) {
        CBFAssert(history.size() > 0);
        // Find the common ancestor, if any. Along the way, preflight revision IDs:
        int i;
        unsigned lastGen = 0;
        const Revision* parent = NULL;
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
                parent = _insert(history[i], slice(), parent, false, false);
            _insert(history[0], data, parent, deleted, hasAttachments);
        }
        return commonAncestorIndex;
    }

    unsigned RevTree::prune(unsigned maxDepth) {
        if (maxDepth == 0 || _revs.size() <= maxDepth)
            return 0;

        // First find all the leaves, and walk from each one down to its root:
        int numPruned = 0;
        Revision* rev = &_revs[0];
        for (unsigned i=0; i<_revs.size(); i++,rev++) {
            if (rev->isLeaf()) {
                // Starting from a leaf rev, trace its ancestry to find its depth:
                unsigned depth = 0;
                for (Revision* anc = rev; anc; anc = (Revision*)anc->parent()) {
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
        Revision* rev = (Revision*)get(leafID);
        if (!rev || !rev->isLeaf())
            return 0;
        do {
            nPurged++;
            rev->revID.size = 0;                    // mark for purge
            const Revision* parent = (Revision*)rev->parent();
            rev->parentIndex = Revision::kNoParent; // unlink from parent
            rev = (Revision*)parent;
        } while (rev && confirmLeaf(rev));
        compact();
        return nPurged;
    }

    void RevTree::compact() {
        // Create a mapping from current to new rev indexes (after removing pruned/purged revs)
		std::vector<uint16_t> map(_revs.size());
        unsigned i = 0, j = 0;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev, ++i) {
            if (rev->revID.size > 0)
                map[i] = (uint16_t)(j++);
            else
                map[i] = Revision::kNoParent;
        }

        // Finally, slide the surviving revs down and renumber their parent indexes:
        Revision* rev = &_revs[0];
        Revision* dst = rev;
        for (i=0; i<_revs.size(); i++,rev++) {
            if (rev->revID.size > 0) {
                if (rev->parentIndex != Revision::kNoParent)
                    rev->parentIndex = map[rev->parentIndex];
                if (dst != rev)
                    *dst = *rev;
                dst++;
            }
        }
        _revs.resize(dst - &_revs[0]);
        _changed = true;
    }

    // Sort comparison function for an array of Revisions. Higher priority comes _first_.
    bool Revision::operator<(const Revision& rev2) const
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
            oldParents[i] = _revs[i].parentIndex;
            _revs[i].parentIndex = i;
        }

        std::sort(_revs.begin(), _revs.end());

        // oldToNew maps old array indexes to new (sorted) ones.
		std::vector<uint16_t> oldToNew(_revs.size());
        for (uint16_t i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i].parentIndex;
            oldToNew[oldIndex] = i;
        }

        // Now fix up the parentIndex values by running them through oldToNew:
        for (unsigned i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i].parentIndex;
            uint16_t parent = oldParents[oldIndex];
            if (parent != Revision::kNoParent)
                parent = oldToNew[parent];
                _revs[i].parentIndex = parent;
                }
        _sorted = true;
    }

#if DEBUG
    std::string RevTree::dump() {
        std::stringstream out;
        dump(out);
        return out.str();
    }

    void RevTree::dump(std::ostream& out) {
        int i = 0;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            out << "\t" << (++i) << ": ";
            rev->dump(out);
            out << "\n";
        }
    }
#endif

}
