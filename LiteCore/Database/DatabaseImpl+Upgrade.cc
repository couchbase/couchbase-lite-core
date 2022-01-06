//
// DatabaseImpl+Upgrade.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "DatabaseImpl.hh"
#include "CollectionImpl.hh"
#include "VectorRecord.hh"
#include "RecordEnumerator.hh"
#include "RevID.hh"
#include "RevTreeRecord.hh"
#include "VersionVector.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include <inttypes.h>
#include <vector>

namespace litecore {
    using namespace std;

    
    // The fake peer/source ID used for versions migrated from revIDs.
    static constexpr peerID kLegacyPeerID {0x7777777};

    static constexpr const char* kNameOfVersioning[3] = {
        "v2.x rev-trees", "v3.x rev-trees", "version vectors"};


    static void upgradeToVersionVectors(DatabaseImpl*, const Record&, RevTreeRecord&, ExclusiveTransaction&);
    static pair<alloc_slice, alloc_slice>
    upgradeRemoteRevs(DatabaseImpl*, Record, RevTreeRecord&, alloc_slice currentVersion);


    static const Rev* commonAncestor(const Rev *a, const Rev *b) {
        if (a && b) {
            for (auto rev = b; rev; rev = rev->parent) {
                if (rev->isAncestorOf(a))
                    return rev;
            }
        }
        return nullptr;
    }


    void DatabaseImpl::upgradeDocumentVersioning(C4DocumentVersioning curVersioning,
                                                 C4DocumentVersioning newVersioning,
                                                 ExclusiveTransaction &t)
    {
        if (newVersioning == curVersioning)
            return;
        if (newVersioning < curVersioning)
            error::_throw(error::Unimplemented, "Cannot downgrade document versioning");
        if (_config.flags & (kC4DB_ReadOnly |kC4DB_NoUpgrade))
            error::_throw(error::CantUpgradeDatabase, "Document versioning needs upgrade");

        alloc_slice path = getPath();
        LogTo(DBLog, "*** Upgrading stored documents from %s to %s ***",
              kNameOfVersioning[curVersioning], kNameOfVersioning[newVersioning]);
        uint64_t docCount = 0;

        // Iterate over all documents in all collections:
        for (string &ksName : _dataFile->allKeyStoreNames()) {
            if (SQLiteDataFile::keyStoreNameIsCollection(ksName)) {
                LogTo(DBLog, "*** Upgrading stored documents in `%s` from %s to %s ***",
                      ksName.c_str(),
                      kNameOfVersioning[curVersioning], kNameOfVersioning[newVersioning]);
                RecordEnumerator::Options options;
                options.sortOption = kUnsorted;
                options.includeDeleted = true;
                options.contentOption = kEntireBody;
                RecordEnumerator e(_dataFile->getKeyStore(ksName), options);
                while (e.next()) {
                    // Read the doc as a RevTreeRecord. This will correctly read both the old 2.x style
                    // record (with no `extra`) and the new 3.x style.
                    const Record &rec = e.record();
                    RevTreeRecord revTree(defaultKeyStore(), rec);
                    if (newVersioning == kC4VectorVersioning) {
                        // Upgrade from rev-trees (v2 or v3) to version-vectors:
                        upgradeToVersionVectors(this, rec, revTree, t);
                    } else {
                        // Upgrading v2 rev-trees to new db schema with `extra` column;
                        // simply resave and RevTreeRecord will use the new schema:
                        auto result = revTree.save(t);
                        Assert(result == RevTreeRecord::kNoNewSequence);
                        LogVerbose(DBLog, "  - Upgraded doc '%.*s' #%s",
                                SPLAT(rec.key()),
                                revid(rec.version()).str().c_str());
                    }

                    ++docCount;
                }
            }
        }
        LogTo(DBLog, "\t%" PRIu64 " documents upgraded, now committing changes...", docCount);
    }


    // Upgrades a Record from rev-trees to version vectors.
    static void upgradeToVersionVectors(DatabaseImpl *db,
                                        const Record &rec,
                                        RevTreeRecord &revTree,
                                        ExclusiveTransaction &t)
    {
        // Upgrade from rev-trees (v2 or v3) to version-vectors:
        auto currentRev = revTree.currentRevision();
        auto remoteRev = revTree.latestRevisionOnRemote(RevTree::kDefaultRemoteID);
        auto baseRev = commonAncestor(currentRev, remoteRev);

        // Create a version vector:
        // - If there's a remote base revision, use its generation with the legacy peer ID.
        // - Add the current rev's generation (relative to the remote base, if any)
        //   with the local 'me' peer ID.
        VersionVector vv;
        int localChanges = int(currentRev->revID.generation());
        if (baseRev) {
            vv.add({baseRev->revID.generation(), kLegacyPeerID});
            localChanges -= int(baseRev->revID.generation());
        }
        if (localChanges > 0)
            vv.add({generation(localChanges), kMePeerID});
        auto binaryVersion = vv.asBinary();

        // Propagate any saved remote revisions to the new document:
        slice body;
        alloc_slice allocedBody, extra;
        if (revTree.remoteRevisions().empty()) {
            body = currentRev->body();
        } else {
            tie(allocedBody, extra) = upgradeRemoteRevs(db, rec, revTree, binaryVersion);
            body = allocedBody;
        }

        // Now save:
        RecordUpdate newRec(revTree.docID(), body, revTree.flags());
        newRec.extra = extra;
        newRec.version = binaryVersion;
        newRec.sequence = revTree.sequence();
        newRec.subsequence = revTree.record().subsequence();
        //TODO: Find conflicts and add them to newRec.extra
        Assert(db->defaultKeyStore().set(newRec, false, t) > 0_seq);

        LogVerbose(DBLog, "  - Upgraded doc '%.*s', %s -> [%s], %zu bytes body, %zu bytes extra",
                SPLAT(rec.key()),
                revid(rec.version()).str().c_str(),
                string(vv.asASCII()).c_str(),
                newRec.body.size, newRec.extra.size);
    }


    // Subroutine that does extra work to upgrade a doc with remote-tagged revs to version vectors.
    static pair<alloc_slice, alloc_slice> upgradeRemoteRevs(DatabaseImpl *db,
                                                            Record rec,
                                                            RevTreeRecord &revTree,
                                                            alloc_slice currentVersion)
    {
        // Make an in-memory VV-based Record, with no remote revisions:
        const Rev *currentRev = revTree.currentRevision();
        rec.setVersion(currentVersion);
        rec.setBody(currentRev->body());
        rec.setExtra(nullslice);

        // Instantiate a VectorRecord for this document, without reading the database:
        VectorRecord nuDoc(db->defaultKeyStore(), Versioning::RevTrees, rec);
        nuDoc.setEncoder(db->sharedFleeceEncoder());

        // Add each remote revision:
        for (auto i = revTree.remoteRevisions().begin(); i != revTree.remoteRevisions().end(); ++i) {
            auto remoteID = RemoteID(i->first);
            const Rev *rev = i->second;
            Revision nuRev;
            alloc_slice binaryVers;
            if (rev == currentRev) {
                nuRev = nuDoc.currentRevision();
            } else {
                if (rev->body())
                    nuRev.properties = fleece::Value::fromData(rev->body(), kFLTrusted).asDict();
                nuRev.flags = {};
                if (rev->flags & Rev::kDeleted)        nuRev.flags |= DocumentFlags::kDeleted;
                if (rev->flags & Rev::kHasAttachments) nuRev.flags |= DocumentFlags::kHasAttachments;

                VersionVector vv;
                vv.add({rev->revID.generation(), kLegacyPeerID});
                binaryVers = vv.asBinary();
                nuRev.revID = revid(binaryVers);
            }
            nuDoc.setRemoteRevision(remoteID, nuRev);
        }

        return nuDoc.encodeBodyAndExtra();
    }


}
