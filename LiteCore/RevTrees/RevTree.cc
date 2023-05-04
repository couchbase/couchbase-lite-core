//
// RevTree.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

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
#    include <iostream>
#    include <sstream>
#endif


namespace litecore {
    using namespace fleece;

    static bool compareRevs(const Rev* rev1, const Rev* rev2);

    RevTree::RevTree(slice body, slice extra, sequence_t seq) { decode(body, extra, seq); }

    RevTree::RevTree(const RevTree& other)
        : _sorted(other._sorted), _changed(other._changed), _unknown(other._unknown), _pruneDepth(other._pruneDepth) {
        // It's important to have _revs in the same order as other._revs.
        // That means we can't just copy other._revsStorage to _revsStorage;
        // we have to copy _revs in order:
        _revs.reserve(other._revs.size());
        for ( const Rev* otherRev : other._revs ) {
            // We need to keep the data in _insertedData alive that is referenced
            // by Rev.revID and Rev._body of the Revs we copy.
            // Because Revs can be purged and bodies can be removed from parent Revs,
            // not all of the data in other._insertedData will be referenced from other._revs.
            // That is why we don't just copy other._insertedData. Instead we keep track
            // of which data of a Rev is owned by _insertedData and only add that data
            // to the new _insertedData.
            // Otherwise, when a document is repeatedly updated, data would accumulate,
            // occupying memory and requiring copying + reference counting.
            if ( otherRev->_hasInsertedRevID ) {
                auto revID = (alloc_slice*)&(otherRev->revID);
                _insertedData.push_back(*revID);
            }
            if ( otherRev->_hasInsertedBody ) {
                auto body = (alloc_slice*)&otherRev->_body;
                _insertedData.push_back(*body);
            }

            _revsStorage.emplace_back(*otherRev);
            _revs.push_back(&_revsStorage.back());
        }
        // Fix up the newly copied Revs so they point to me (and my other Revs), not other:
        for ( Rev* rev : _revs ) {
            if ( rev->parent ) rev->parent = _revs[rev->parent->index()];
            rev->owner = this;
        }
        // Copy _remoteRevs:
        for ( auto& i : other._remoteRevs ) { _remoteRevs[i.first] = _revs[i.second->index()]; }
    }

    void RevTree::decode(slice body, slice extra, sequence_t seq) {
        // In 2.0 schema, entire tree is stored in `body` and there is no `extra`.
        // In 3.0 schema, the rev tree is in `extra`, except the current rev's body is in `body`.
        slice rawTree = (extra ? extra : body);
        _revsStorage  = RawRevision::decodeTree(rawTree, _remoteRevs, _rejectedRevs, this, seq);
        initRevs();
        if ( body && extra ) {
            auto cur = currentRevision();
            Assert(cur);
            Assert(!cur->body());
            substituteBody(cur, body);
        }
    }

    void RevTree::initRevs() {
        _revs.resize(_revsStorage.size());
        auto i = _revs.begin();
        for ( Rev& rev : _revsStorage ) {
            *i = &rev;
            ++i;
        }
    }

    pair<slice, alloc_slice> RevTree::encode() {
        sort();
        const Rev* cur = currentRevision();
        slice      curBody;
        if ( cur ) {
            curBody = cur->body();
            substituteBody(cur, nullslice);
        }
        alloc_slice tree = RawRevision::encodeTree(_revs, _remoteRevs, _rejectedRevs);
        if ( cur ) substituteBody(cur, curBody);
        return {curBody, tree};
    }

#if DEBUG
    void Rev::dump(std::ostream& out) {
        out << "(" << uint64_t(sequence) << ") " << (std::string)revID.expanded() << "  ";
        if ( isLeaf() ) out << " leaf";
        if ( isDeleted() ) out << " del";
        if ( hasAttachments() ) out << " attachments";
        if ( isNew() ) out << " (new)";
    }
#endif

#pragma mark - ACCESSORS:

    const Rev* RevTree::currentRevision() const {
        Assert(!_unknown);
        const_cast<RevTree*>(this)->sort();
        return _revs.empty() ? nullptr : _revs[0];
    }

    const Rev* RevTree::get(unsigned index) const {
        Assert(!_unknown);
        Assert(index < _revs.size());
        return _revs[index];
    }

    const Rev* RevTree::get(revid revID) const {
        for ( Rev* rev : _revs ) {
            if ( rev->revID == revID ) return rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    const Rev* RevTree::getBySequence(sequence_t seq) const {
        for ( Rev* rev : _revs ) {
            if ( rev->sequence == seq ) return rev;
        }
        Assert(!_unknown);
        return nullptr;
    }

    bool RevTree::hasConflict() const {
        if ( _revs.size() < 2 ) {
            Assert(!_unknown);
            return false;
        } else if ( _sorted ) {
            return _revs[1]->isActive();
        } else {
            unsigned nActive = 0;
            for ( Rev* rev : _revs ) {
                if ( rev->isActive() ) {
                    if ( ++nActive > 1 ) return true;
                }
            }
            return false;
        }
    }

    bool Rev::isActive() const {
        // "Active" revs contribute to conflicts, or rather, a conflict is when there is more than
        // one active rev.
        // Traditionally (back to CouchDB) an active rev is one that's a leaf and not a deletion.
        // Deleted revs are excluded because they are used to cap conflicting branches, so they
        // shouldn't create conflicts themselves.
        // However, with no-conflicts servers in CBL 2, we do want to allow a conflict between a
        // live and a deleted document. So we treat a deletion as active if it's the server's
        // current revision.
        return isLeaf() && (!isDeleted() || isLatestRemoteRevision());
    }

    slice Rev::body() const {
        slice body = _body;
        if ( (size_t)body.buf & 1 ) {
            // Fleece data must be 2-byte-aligned, so we have to copy body to the heap:
            auto xthis              = const_cast<Rev*>(this);
            auto xowner             = const_cast<RevTree*>(owner);
            xthis->_hasInsertedBody = true;
            body = xthis->_body = (slice)xowner->copyBody(_body);
        }
        return body;
    }

    unsigned Rev::index() const {
        auto& revs = owner->_revs;
        auto  i    = find(revs.begin(), revs.end(), this);
        Assert(i != revs.end());
        return (unsigned)(i - revs.begin());
    }

    const Rev* Rev::next() const {
        auto i = index() + 1;
        return i < owner->size() ? owner->get(i) : nullptr;
    }

    std::vector<const Rev*> Rev::history() const {
        std::vector<const Rev*> h;
        for ( const Rev* rev = this; rev; rev = rev->parent ) h.push_back(rev);
        return h;
    }

    bool Rev::isAncestorOf(const Rev* rev) const {
        do {
            if ( rev == this ) return true;
            rev = rev->parent;
        } while ( rev );
        return false;
    }

    bool Rev::isLatestRemoteRevision() const { return owner->isLatestRemoteRevision(this); }

    bool RevTree::isBodyOfRevisionAvailable(const Rev* rev) const {
        return rev->_body.buf != nullptr;  // RevTreeRecord overrides this
    }

    bool RevTree::confirmLeaf(Rev* testRev) {
        for ( Rev* rev : _revs )
            if ( rev->parent == testRev ) return false;
        testRev->addFlag(Rev::kLeaf);
        return true;
    }

    pair<Rev*, int> RevTree::findCommonAncestor(const std::vector<revidBuffer> history, bool allowConflict) {
        Assert(history.size() > 0);
        unsigned lastGen      = 0;
        Rev*     parent       = nullptr;
        size_t   historyCount = history.size();
        int      i            = 0;
        for ( i = 0; i < historyCount; i++ ) {
            unsigned gen = history[i].generation();
            if ( lastGen > 0 && gen != lastGen - 1 ) {
                // Generation numbers not in sequence:
                if ( gen < lastGen && i >= _pruneDepth - 1 ) {
                    // As a special case, allow this gap in the history as long as it's at a depth
                    // that's going to be pruned away anyway. This allows very long histories to
                    // be represented in short form by skipping revs in the middle.
                    ;
                } else {
                    // Otherwise this is an error.
                    return {nullptr, -400};
                }
            }
            lastGen = gen;

            parent = (Rev*)get(history[i]);
            if ( parent ) break;
        }

        if ( !allowConflict ) {
            if ( (parent && !parent->isLeaf()) || (!parent && !_revs.empty()) ) return {nullptr, -409};
        }

        return {parent, i};
    }

#pragma mark - INSERTION:

    alloc_slice RevTree::copyBody(slice body) {
        _insertedData.emplace_back(body);
        return _insertedData.back();
    }

    alloc_slice RevTree::copyBody(const alloc_slice& body) {
        if ( body.size == 0 ) return body;
        _insertedData.push_back(body);
        return body;
    }

    // Lowest-level insert method. Does no sanity checking, always inserts.
    Rev* RevTree::_insert(revid unownedRevID, const alloc_slice& body, Rev* parentRev, Rev::Flags revFlags,
                          bool markConflict) {
        revFlags = Rev::Flags(revFlags & (Rev::kDeleted | Rev::kClosed | Rev::kHasAttachments | Rev::kKeepBody));
        Assert(!((revFlags & Rev::kClosed) && !(revFlags & Rev::kDeleted)));

        Assert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.emplace_back(unownedRevID);
        revid revID = revid(_insertedData.back());

        _revsStorage.emplace_back();
        Rev* newRev               = &_revsStorage.back();
        newRev->_hasInsertedRevID = true;
        newRev->_hasInsertedBody  = true;
        newRev->owner             = this;
        newRev->revID             = revID;
        newRev->_body             = (slice)copyBody(body);
        newRev->sequence          = 0_seq;  // Sequence is unknown till record is saved
        newRev->flags             = Rev::Flags(Rev::kLeaf | Rev::kNew | revFlags);
        newRev->parent            = parentRev;

        if ( parentRev ) {
            if ( markConflict && (!parentRev->isLeaf() || parentRev->isConflict()) )
                newRev->addFlag(Rev::kIsConflict);  // Creating or extending a branch
            parentRev->clearFlag(Rev::kLeaf);
            if ( revFlags & Rev::kKeepBody ) keepBody(newRev);
            else if ( revFlags & Rev::kClosed )
                removeBodiesOnBranch(parentRev);  // no bodies on a closed conflict branch
        } else {
            // Root revision:
            if ( markConflict && !_revs.empty() ) newRev->addFlag(Rev::kIsConflict);  // Creating a 2nd root
        }

        _changed = true;
        if ( !_revs.empty() ) _sorted = false;
        _revs.push_back(newRev);
        return newRev;
    }

    const Rev* RevTree::insert(revid revID, const alloc_slice& body, Rev::Flags revFlags, const Rev* parent,
                               bool allowConflict, bool markConflict, int& httpStatus) {
        // Make sure the given revID is valid:
        uint32_t newGen = revID.generation();
        if ( newGen == 0 ) {
            httpStatus = 400;
            return nullptr;
        }

        if ( get(revID) ) {
            httpStatus = 200;
            return nullptr;  // already exists
        }

        // Find the parent rev, if a parent ID is given:
        uint32_t parentGen;
        if ( parent ) {
            if ( !allowConflict && !parent->isLeaf() ) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = parent->revID.generation();
        } else {
            if ( !allowConflict && _revs.size() > 0 ) {
                httpStatus = 409;
                return nullptr;
            }
            parentGen = 0;
        }

        // Enforce that generation number went up by 1 from the parent:
        if ( newGen != parentGen + 1 ) {
            httpStatus = 400;
            return nullptr;
        }

        // Finally, insert:
        httpStatus = (revFlags & Rev::kDeleted) ? 200 : 201;
        return _insert(revID, body, (Rev*)parent, revFlags, markConflict);
    }

    const Rev* RevTree::insert(revid revID, const alloc_slice& body, Rev::Flags revFlags, revid parentRevID,
                               bool allowConflict, bool markConflict, int& httpStatus) {
        const Rev* parent = nullptr;
        if ( parentRevID.buf ) {
            parent = get(parentRevID);
            if ( !parent ) {
                httpStatus = 404;
                return nullptr;  // parent doesn't exist
            }
        }
        return insert(revID, body, revFlags, parent, allowConflict, markConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revidBuffer>& history, const alloc_slice& body, Rev::Flags revFlags,
                               bool allowConflict, bool markConflict) {
        auto [parent, commonAncestorIndex] = findCommonAncestor(history, allowConflict);
        if ( commonAncestorIndex > 0 && body ) {
            // Insert all the new revisions in chronological order:
            for ( int i = commonAncestorIndex - 1; i > 0; --i )
                parent = _insert(history[i], {}, parent, Rev::kNoFlags, markConflict);
            _insert(history[0], body, parent, revFlags, markConflict);
        }
        return commonAncestorIndex;
    }

    void RevTree::markBranchAsNotConflict(const Rev* branch, bool winningBranch) {
        bool keepBodies = winningBranch;
        for ( auto rev = const_cast<Rev*>(branch); rev; rev = const_cast<Rev*>(rev->parent) ) {
            if ( rev->isConflict() ) {
                rev->clearFlag(Rev::kIsConflict);
                _changed = true;
                if ( !winningBranch ) return;  // stop at end of conflicting branch
            }

            if ( rev->keepBody() ) {
                if ( keepBodies ) {
                    keepBodies = false;  // Only one rev on a branch may have kKeepBody
                } else {
                    rev->clearFlag(Rev::kKeepBody);
                    _changed = true;
                }
            }
        }
    }

    void RevTree::resetConflictSequence(const Rev* winningRev) {
        auto rev      = const_cast<Rev*>(winningRev);
        rev->sequence = 0_seq;
    }

#pragma mark - REMOVAL (prune / purge / compact):

    void RevTree::keepBody(const Rev* rev_in) {
        auto rev = const_cast<Rev*>(rev_in);
        rev->addFlag(Rev::kKeepBody);

        // Only one rev in a branch can have the keepBody flag
        bool conflict = rev->isConflict();
        for ( auto ancestor = rev->parent; ancestor; ancestor = ancestor->parent ) {
            if ( conflict && !ancestor->isConflict() ) break;  // stop at end of a conflict branch
            const_cast<Rev*>(ancestor)->clearFlag(Rev::kKeepBody);
        }
        _changed = true;
    }

    void RevTree::removeBody(const Rev* rev) {
        if ( rev->body() ) {
            const_cast<Rev*>(rev)->removeBody();
            _changed = true;
        }
    }

    void RevTree::removeBodiesOnBranch(const Rev* rev) {
        do {
            removeBody(rev);
            rev = rev->parent;
        } while ( rev );
    }

    // Remove bodies of already-saved revs that are no longer leaves:
    void RevTree::removeNonLeafBodies() {
        for ( Rev* rev : _revs ) {
            if ( rev->_body.size > 0 && !(rev->flags & (Rev::kLeaf | Rev::kNew | Rev::kKeepBody)) ) {
                rev->removeBody();
                _changed = true;
            }
        }
    }

    unsigned RevTree::prune(unsigned maxDepth) {
        Assert(maxDepth > 0);
        if ( _revs.size() <= maxDepth ) return 0;

        // First find all the leaves, and walk from each one down to its root:
        int numPruned = 0;
        for ( auto& rev : _revs ) {
            if ( rev->isLeaf() ) {
                // Starting from a leaf rev, trace its ancestry to find its depth:
                unsigned depth = 0;
                for ( Rev* anc = rev; anc; anc = (Rev*)anc->parent ) {
                    if ( ++depth > maxDepth && !anc->keepBody() ) {
                        // Mark revs that are too far away:
                        anc->addFlag(Rev::kPurge);
                        numPruned++;
                    }
                }
            } else if ( _sorted ) {
                break;
            }
        }

        if ( numPruned == 0 ) return 0;

        // Don't prune current remote revisions:
        for ( auto& r : _remoteRevs ) {
            if ( r.second->isMarkedForPurge() ) {
                const_cast<Rev*>(r.second)->clearFlag(Rev::kPurge);
                --numPruned;
            }
        }

        if ( numPruned == 0 ) return 0;

        // Clear parent links that point to revisions being pruned:
        for ( auto& rev : _revs ) {
            if ( !rev->isMarkedForPurge() ) {
                while ( rev->parent && rev->parent->isMarkedForPurge() ) rev->parent = rev->parent->parent;
            }
        }
        compact();
        return numPruned;
    }

    int RevTree::purge(revid leafID) {
        int  nPurged = 0;
        Rev* rev     = (Rev*)get(leafID);
        if ( !rev || !rev->isLeaf() ) return 0;
        do {
            nPurged++;
            rev->addFlag(Rev::kPurge);
            const Rev* parent = (Rev*)rev->parent;
            rev->parent       = nullptr;  // unlink from parent
            rev               = (Rev*)parent;
        } while ( rev && confirmLeaf(rev) );
        compact();
        checkForResolvedConflict();
        return nPurged;
    }

    int RevTree::purgeAll() {
        int result = (int)_revs.size();
        _revs.resize(0);
        _changed = true;
        _sorted  = true;
        return result;
    }

    void RevTree::compact() {
        // Slide the surviving revs down:
        auto dst = _revs.begin();
        for ( auto rev = dst; rev != _revs.end(); rev++ ) {
            if ( !(*rev)->isMarkedForPurge() ) {
                if ( dst != rev ) *dst = *rev;
                dst++;
            }
        }
        _revs.resize(dst - _revs.begin());

        // Remove purged revs from _remoteRevs:
        auto tempRemoteRevs = _remoteRevs;
        for ( auto& e : tempRemoteRevs ) {
            if ( e.second->isMarkedForPurge() ) _remoteRevs.erase(e.first);
        }
        // Remove purged revs from _rejectedRevs
        auto tempRejectedRevs = _rejectedRevs;
        _rejectedRevs.clear();
        for ( auto& e : tempRejectedRevs ) {
            if ( !e->isMarkedForPurge() ) _rejectedRevs.push_back(e);
        }

        _changed = true;
    }

#pragma mark - SORT / SAVE:

    // Sort comparison function for an array of Revisions. Higher priority comes _first_, so this
    // is a descending sort. The function returns true if rev1 is higher priority than rev2.
    __hot static bool compareRevs(const Rev* rev1, const Rev* rev2) {
        // Leaf revs go before non-leaves.
        int delta = rev2->isLeaf() - rev1->isLeaf();
        if ( delta ) return delta < 0;
        // Conflicting revs never go first.
        delta = rev1->isConflict() - rev2->isConflict();
        if ( delta ) return delta < 0;
        // Live revs go before deletions.
        delta = rev1->isDeleted() - rev2->isDeleted();
        if ( delta ) return delta < 0;
        // Closed revs come after even deletions
        delta = rev1->isClosed() - rev2->isClosed();
        if ( delta ) return delta < 0;
        // Otherwise compare rev IDs, with higher rev ID going first:
        return rev2->revID < rev1->revID;
    }

    void RevTree::sort() {
        if ( _sorted ) return;
        std::sort(_revs.begin(), _revs.end(), &compareRevs);
        _sorted = true;
        checkForResolvedConflict();
    }

    // If there are no non-conflict leaves, remove the conflict marker from the 1st:
    void RevTree::checkForResolvedConflict() {
        if ( _sorted && !_revs.empty() && _revs[0]->isConflict() ) markBranchAsNotConflict(_revs[0], true);
    }

    bool RevTree::hasNewRevisions() const {
        for ( Rev* rev : _revs ) {
            if ( rev->isNew() || rev->sequence == 0_seq ) return true;
        }
        return false;
    }

    void RevTree::saved(sequence_t newSequence) {
        for ( Rev* rev : _revs ) {
            rev->clearFlag(Rev::kNew);
            if ( rev->sequence == 0_seq ) { rev->sequence = newSequence; }
        }
    }

#pragma mark - ETC.

    bool RevTree::isLatestRemoteRevision(const Rev* rev) const {
        for ( auto& r : _remoteRevs ) {
            if ( r.second == rev ) return true;
        }
        return false;
    }

    void RevTree::revIsRejected(const Rev* rev) {
        Assert(rev);
        if ( std::find(_rejectedRevs.begin(), _rejectedRevs.end(), rev) == _rejectedRevs.end() ) {
            _rejectedRevs.push_back(rev);
            _changed = true;
        }
    }

    const Rev* RevTree::latestRevisionOnRemote(RemoteID remote) {
        Assert(remote != kNoRemoteID);
        auto i = _remoteRevs.find(remote);
        if ( i == _remoteRevs.end() ) return nullptr;
        return i->second;
    }

    void RevTree::setLatestRevisionOnRemote(RemoteID remote, const Rev* rev) {
        Assert(remote != kNoRemoteID);
        if ( rev ) {
            _remoteRevs[remote] = rev;
        } else {
            _remoteRevs.erase(remote);
        }
        _changed = true;
    }


#if DEBUG
    void RevTree::dump() { dump(std::cerr); }

    void RevTree::dump(std::ostream& out) {
        int i = 0;
        for ( Rev* rev : _revs ) {
            out << "\t" << (++i) << ": ";
            rev->dump(out);

            for ( auto& e : _remoteRevs ) {
                if ( e.second == rev ) out << " <--remote#" << e.first;
            }

            out << "\n";
        }
    }
#endif

}  // namespace litecore
