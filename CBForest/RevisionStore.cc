//
//  RevisionStore.cc
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RevisionStore.hh"
#include "Revision.hh"
#include "DocEnumerator.hh"
#include "VarInt.hh"

namespace cbforest {


    // Separates the docID and the author in the keys of non-current Revisions.
    static const char kDocIDDelimiter ='\t';

    // Separates the author and generation in the keys of non-current Revisions.
    static const char kAuthorDelimiter = ',';


    RevisionStore::RevisionStore(Database *db)
    :_db(db),
     _store(db->defaultKeyStore()),
     _nonCurrentStore(db->getKeyStore("revs"))
    { }


#pragma mark - GET:


    // Get the current revision of a document
    Revision::Ref RevisionStore::get(slice docID, KeyStore::contentOptions opt) const {
        Document doc(docID);
        if (!_store.read(doc, opt))
            return nullptr;
        return Revision::Ref{ new Revision(std::move(doc)) };
    }


    Revision::Ref RevisionStore::get(slice docID, slice revID, KeyStore::contentOptions opt) const {
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
    Revision::Ref RevisionStore::getNonCurrent(slice docID, slice revID,
                                           KeyStore::contentOptions opt) const
    {
        CBFAssert(revID.size > 0);
        Document doc(keyForNonCurrentRevision(docID, version{revID}));
        if (!_nonCurrentStore.read(doc, opt))
            return nullptr;
        return Revision::Ref{ new Revision(std::move(doc)) };
    }


    // Make sure a Revision has a body (if it was originally loaded as meta-only)
    void RevisionStore::readBody(cbforest::Revision &rev) {
        if (rev.document().body().buf == nullptr) {
            KeyStore &store = rev.isCurrent() ? _store : _nonCurrentStore;
            store.readBody(rev.document());
        }
    }


    versionOrder RevisionStore::checkRevision(slice docID, slice revID) {
        CBFAssert(revID.size);
        version checkVers(revID);
        auto rev = get(docID);
        if (rev) {
            auto order = checkVers.compareTo(rev->version());
            if (order != kOlder)
                return order;    // Current revision is equal or newer
            if (rev->isConflicted()) {
                auto e = enumerateRevisions(docID);
                while (e.next()) {
                    Revision conflict(e.moveDoc());
                    order = checkVers.compareTo(conflict.version());
                    if (order != kOlder)
                        return order;
                }
            }
        }
        return kOlder;
    }


#pragma mark - PUT:


    Revision::Ref RevisionStore::create(slice docID,
                                        const VersionVector &parentVersion,
                                        Revision::BodyParams body,
                                        Transaction &t)
    {
        // Check for conflict, and compute new version-vector:
        auto current = get(docID, KeyStore::kMetaOnly);
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



    versionOrder RevisionStore::insert(Revision &newRev, Transaction &t) {
        auto current = get(newRev.docID(), KeyStore::kMetaOnly);
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
                // Oops, they conflict:
                CBFAssert(false);//TODO
                break;
        }
        return cmp;
    }


    Revision::Ref RevisionStore::resolveConflict(std::vector<Revision*> conflicting,
                                                 Revision::BodyParams body,
                                                 Transaction &t)
    {
        return resolveConflict(conflicting, slice::null, body, t);
        // CASRevisionStore overrides this
    }

    Revision::Ref RevisionStore::resolveConflict(std::vector<Revision*> conflicting,
                                                 slice keepingRevID,
                                                 Revision::BodyParams body,
                                                 Transaction &t)
    {
        CBFAssert(conflicting.size() >= 2);
        VersionVector newVersion;
        Revision* current = NULL;
        for (auto rev : conflicting) {
            newVersion = newVersion.mergedWith(rev->version());
            if (rev->isCurrent())
                current = rev;
            else if (rev->revID() != keepingRevID)
                t(_nonCurrentStore).del(rev->document());
        }
        CBFAssert(current != NULL);

        auto newRev = Revision::Ref{ new Revision(conflicting[0]->docID(), newVersion,
                                                  body, true) };
        t(_store).write(newRev->document());
        return newRev;
    }


    // Replace the current revision `current` with `newRev`
    void RevisionStore::replaceCurrent(Revision &newRev, Revision *current, Transaction &t) {
        if (current) {
            willReplaceCurrentRevision(*current, newRev, t);
            if (current->isConflicted())
                deleteAncestors(newRev, t);
        }
        newRev.setCurrent(true);    // update key to just docID
        t(_store).write(newRev.document());
    }


    bool RevisionStore::deleteNonCurrent(slice docID, slice revID, Transaction &t) {
        return t(_nonCurrentStore).del(keyForNonCurrentRevision(docID, version(revID)));
    }


#pragma mark - ENUMERATION:


    static const DocEnumerator::Options kRevEnumOptions = {
        0,
        UINT_MAX,
        false,
        false,  // no inclusiveStart
        false,  // no inclusiveEnd
        false,
        KeyStore::kMetaOnly
    };


    DocEnumerator RevisionStore::enumerateRevisions(slice docID, slice author) {
        return DocEnumerator(_nonCurrentStore,
                             startKeyFor(docID, author),
                             endKeyFor(docID, author),
                             kRevEnumOptions);
    }


    void RevisionStore::deleteAncestors(Revision &child, Transaction &t) {
        DocEnumerator e = enumerateRevisions(child.docID());
        while (e.next()) {
            Revision rev(e.moveDoc());
            if (rev.version().compareTo(child.version()) == kOlder
                    && !shouldKeepAncestor(rev, child)) {
                t(_nonCurrentStore).del(rev.document());
            }
        }
    }


#pragma mark DOC ID / KEYS:


    // Concatenates the docID, the author and the generation (with delimiters).
    // author and generation are optional.
    static alloc_slice mkkey(slice docID, peerID author, generation gen) {
        size_t size = docID.size + 1;
        if (author.buf) {
            size += author.size + 1;
            if (gen > 0)
                size += SizeOfVarInt(gen);
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

    alloc_slice RevisionStore::keyForNonCurrentRevision(slice docID, struct version vers) {
        return mkkey(docID, vers.author, vers.gen);
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

    bool RevisionStore::shouldKeepAncestor(const Revision &rev, const Revision &child) {
        return false;
    }

}
