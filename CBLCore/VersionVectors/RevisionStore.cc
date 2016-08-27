//
//  RevisionStore.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "RevisionStore.hh"
#include "Revision.hh"
#include "Error.hh"
#include "DocEnumerator.hh"
#include "varint.hh"

/** IMPLEMENTATION NOTES:
 
    RevisionStore uses two KeyStores:

    `_currentStore` (the database's default KeyStore) stores only current revisions.
    The key is the exact document ID; meta contains the flags, version vector and document type;
    and the body is the document body (JSON or Fleece; CBL Core currently doesn't care.)
 
    `nonCurrentStore` (named "revs") stores non-current revisions. These are usually conflicts,
    but if using CAS this also contains the server ancestor of the current revision.
    The key is the docID plus revID; meta and body are the same as in `_currentStore`.
 */

namespace CBL_Core {


    // Separates the docID and the author in the keys of non-current Revisions.
    static const char kDocIDDelimiter ='\t';

    // Separates the author and generation in the keys of non-current Revisions.
    static const char kAuthorDelimiter = ',';


    RevisionStore::RevisionStore(DataFile *db, peerID myPeerID)
    :_currentStore(db->defaultKeyStore()),
     _nonCurrentStore(db->getKeyStore("revs")),
     _myPeerID(myPeerID)
    { }


#pragma mark - GET:


    // Get the current revision of a document
    Revision::Ref RevisionStore::get(slice docID, ContentOptions opt) const {
        Document doc(docID);
        if (!_currentStore.read(doc, opt))
            return nullptr;
        return Revision::Ref{ new Revision(std::move(doc)) };
    }


    Revision::Ref RevisionStore::get(slice docID, slice revID, ContentOptions opt) const {
        // No revID means current revision:
        if (revID.size == 0)
            return get(docID, opt);

        // Look in non-current revision store:
        auto rev = getNonCurrent(docID, revID, opt);
        if (rev)
            return rev;

        // Not found; see if it's the current revision:
        rev = get(docID, opt);
        if (rev && rev->revID() == revID)
            return rev;
        return nullptr;
    }


    // Get a revision from the _nonCurrentStore only
    Revision::Ref RevisionStore::getNonCurrent(slice docID, slice revID, ContentOptions opt) const {
        CBFAssert(revID.size > 0);
        Document doc(keyForNonCurrentRevision(docID, Version{revID}));
        if (!_nonCurrentStore.read(doc, opt))
            return nullptr;
        return Revision::Ref{ new Revision(std::move(doc)) };
    }


    // Make sure a Revision has a body (if it was originally loaded as meta-only)
    void RevisionStore::readBody(CBL_Core::Revision &rev) {
        KeyStore &store = rev.isCurrent() ? _currentStore : _nonCurrentStore;
        store.readBody(rev.document());
    }


    // How does this revision compare to what's in the database?
    versionOrder RevisionStore::checkRevision(slice docID, slice revID) {
        CBFAssert(revID.size);
        Version checkVers(revID);
        auto rev = get(docID);
        if (rev) {
            auto order = checkVers.compareTo(rev->version());
            if (order != kOlder)
                return order;    // Current revision is equal or newer
            if (rev->isConflicted()) {
                auto e = enumerateRevisions(docID);
                while (e.next()) {
                    Revision conflict(e.doc());
                    order = checkVers.compareTo(conflict.version());
                    if (order != kOlder)
                        return order;
                }
            }
        }
        return kOlder;
    }


#pragma mark - PUT:


    // Creates a new revision.
    Revision::Ref RevisionStore::create(slice docID,
                                        const VersionVector &parentVersion,
                                        Revision::BodyParams body,
                                        Transaction &t)
    {
        // Check for conflict, and compute new version-vector:
        auto current = get(docID, kMetaOnly);
        VersionVector newVersion;
        if (current)
            newVersion = current->version();
        if (parentVersion != newVersion)
            return nullptr;
        newVersion.incrementGen(kMePeerID);

        auto newRev = Revision::Ref{ new Revision(docID, newVersion, body, true) };
        replaceCurrent(*newRev, current.get(), t);
        return newRev;
    }


    // Inserts a revision, probably from a peer.
    versionOrder RevisionStore::insert(Revision &newRev, Transaction &t) {
        auto current = get(newRev.docID(), kMetaOnly);
        auto cmp = current ? newRev.version().compareTo(current->version()) : kNewer;
        switch (cmp) {
            case kSame:
            case kOlder:
                // This revision already exists, or is obsolete: no-op
                break;

            case kNewer:
                // This revision is newer than the current one, so replace it:
                replaceCurrent(newRev, current.get(), t);
                break;

            case kConflicting:
                // Oops, it conflicts. Delete any saved revs that are ancestors of it,
                // then save it to the non-current store and mark the current rev as conflicted:
                deleteAncestors(newRev, t);
                newRev.setCurrent(false);
                newRev.setConflicted(true);
                _nonCurrentStore.write(newRev.document(), t);
                markConflicted(*current, true, t);
                break;
        }
        return cmp;
    }


    // Creates a new revision that resolves a conflict.
    Revision::Ref RevisionStore::resolveConflict(std::vector<Revision*> conflicting,
                                                 Revision::BodyParams body,
                                                 Transaction &t)
    {
        return resolveConflict(conflicting, slice::null, body, t);
        // CASRevisionStore overrides this
    }

    Revision::Ref RevisionStore::resolveConflict(std::vector<Revision*> conflicting,
                                                 slice keepRevID,
                                                 Revision::BodyParams bodyParams,
                                                 Transaction &t)
    {
        CBFAssert(conflicting.size() >= 2);
        VersionVector newVersion;
        Revision* current = NULL;
        for (auto rev : conflicting) {
            newVersion = newVersion.mergedWith(rev->version());
            if (rev->isCurrent())
                current = rev;
            else if (rev->revID() != keepRevID)
                _nonCurrentStore.del(rev->document(), t);
        }
        if (!current)
            error::_throw(error::InvalidParameter); // Merge must include current revision
        newVersion.insertMergeRevID(_myPeerID, bodyParams.body);

        slice docID = conflicting[0]->docID();
        bodyParams.conflicted = hasConflictingRevisions(docID);
        auto newRev = Revision::Ref{ new Revision(docID, newVersion, bodyParams, true) };
        _currentStore.write(newRev->document(), t);
        return newRev;
    }


    void RevisionStore::markConflicted(Revision &current, bool conflicted, Transaction &t) {
        if (current.setConflicted(conflicted)) {
            _currentStore.readBody(current.document());
            _currentStore.write(current.document(), t);
            //OPT: This is an expensive way to set a single flag, and it bumps the sequence too
        }
    }


    void RevisionStore::purge(slice docID, Transaction &t) {
        if (_currentStore.del(docID, t)) {
            DocEnumerator e = enumerateRevisions(docID);
            while (e.next())
                _nonCurrentStore.del(e.doc(), t);
        }
    }


    // Replace the current revision `current` with `newRev`
    void RevisionStore::replaceCurrent(Revision &newRev, Revision *current, Transaction &t) {
        if (current) {
            willReplaceCurrentRevision(*current, newRev, t);
            if (current->isConflicted())
                deleteAncestors(newRev, t);
        }
        newRev.setCurrent(true);    // update key to just docID
        _currentStore.write(newRev.document(), t);
    }


    bool RevisionStore::deleteNonCurrent(slice docID, slice revID, Transaction &t) {
        return _nonCurrentStore.del(keyForNonCurrentRevision(docID, Version(revID)), t);
    }


#pragma mark - ENUMERATION:


    static const DocEnumerator::Options kRevEnumOptions = {
        0,
        UINT_MAX,
        false,
        false,  // no inclusiveStart
        false,  // no inclusiveEnd
        false,
        kMetaOnly
    };


    DocEnumerator RevisionStore::enumerateRevisions(slice docID, slice author) {
        return DocEnumerator(_nonCurrentStore,
                             startKeyFor(docID, author),
                             endKeyFor(docID, author),
                             kRevEnumOptions);
    }


    std::vector<std::shared_ptr<Revision> > RevisionStore::allOtherRevisions(slice docID) {
        std::vector<std::shared_ptr<Revision> > revs;
        DocEnumerator e = enumerateRevisions(docID);
        while (e.next()) {
            revs.push_back(std::shared_ptr<Revision>(new Revision(e.doc())));
        }
        return revs;
    }


    void RevisionStore::deleteAncestors(Revision &child, Transaction &t) {
        DocEnumerator e = enumerateRevisions(child.docID());
        while (e.next()) {
            Revision rev(e.doc());
            if (rev.version().compareTo(child.version()) == kOlder
                    && !shouldKeepAncestor(rev)) {
                _nonCurrentStore.del(rev.document(), t);
            }
        }
    }


    bool RevisionStore::hasConflictingRevisions(slice docID) {
        DocEnumerator e = enumerateRevisions(docID);
        while (e.next()) {
            Revision rev(e.doc());
            if (!shouldKeepAncestor(rev))
                return true;
        }
        return false;
    }


#pragma mark DOC ID / KEYS:


    // Concatenates the docID, the author and the generation (with delimiters).
    // author and generation are optional.
    static alloc_slice mkkey(slice docID, peerID author, generation gen) {
        size_t size = docID.size + 1;
        if (author.buf) {
            size += author.size + 1;
            if (gen > 0)
                size += fleece::SizeOfVarInt(gen);
        }
        alloc_slice result(size);
        slice out = result;
        out.writeFrom(docID);
        out.writeByte(kDocIDDelimiter);
        if (author.buf) {
            out.writeFrom(author);
            out.writeByte(kAuthorDelimiter);
            if (gen > 0)
                WriteUVarInt(&out, gen);
        }
        return result;
    }

    alloc_slice RevisionStore::keyForNonCurrentRevision(slice docID, class Version vers) {
        return mkkey(docID, vers.author(), vers.gen());
    }

    alloc_slice RevisionStore::startKeyFor(slice docID, peerID author) {
        return mkkey(docID, author, 0);
    }

    alloc_slice RevisionStore::endKeyFor(slice docID, peerID author) {
        alloc_slice result = mkkey(docID, author, 0);
        const_cast<uint8_t&>(result[result.size-1])++;
        return result;
    }

    slice RevisionStore::docIDFromKey(slice key) {
        auto delim = key.findByte(kDocIDDelimiter);
        if (delim)
            key = key.upTo(delim);
        return key;
    }


#pragma mark TO OVERRIDE:

    // These are no-op stubs. CASRevisionStore implements them.

    void RevisionStore::willReplaceCurrentRevision(Revision &, const Revision &, Transaction &t) {
    }

    bool RevisionStore::shouldKeepAncestor(const Revision &rev) {
        return false;
    }

}
