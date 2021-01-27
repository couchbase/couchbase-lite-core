//
// Database+Upgrade.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Database.hh"
#include "NuDocument.hh"
#include "RecordEnumerator.hh"
#include "RevID.hh"
#include "RevTreeRecord.hh"
#include "VersionVector.hh"
#include "StringUtil.hh"
#include <vector>

namespace c4Internal {
    using namespace std;
    using namespace litecore;

    // The fake peer/source ID used for versions migrated from revIDs.
    static constexpr peerID kLegacyPeerID {0x7777777};

    static pair<alloc_slice, alloc_slice>
    upgradeRemoteRevs(Database*, Record rec, RevTreeRecord&, alloc_slice currentVersion);


    static const Rev* commonAncestor(const Rev *a, const Rev *b) {
        if (a && b) {
            for (auto rev = b; rev; rev = rev->parent) {
                if (rev->isAncestorOf(a))
                    return rev;
            }
        }
        return nullptr;
    }


    void Database::upgradeToVersionVectors(Transaction &transaction) {
        LogTo(DBLog, "*** Upgrading database from rev-trees to version vectors ***");
        uint64_t docCount = 0;

        // Iterate over all documents:
        RecordEnumerator::Options options;
        options.sortOption = kUnsorted;
        options.includeDeleted = true;
        options.contentOption = kEntireBody;
        RecordEnumerator e(defaultKeyStore(), options);
        while (e.next()) {
            // Read the doc as a RevTreeRecord:
            const Record &rec = e.record();
            RevTreeRecord doc(defaultKeyStore(), rec);
            auto currentRev = doc.currentRevision();
            auto remoteRev = doc.latestRevisionOnRemote(RevTree::kDefaultRemoteID);
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
            if (doc.remoteRevisions().empty()) {
                body = currentRev->body();
            } else {
                std::tie(allocedBody, extra) = upgradeRemoteRevs(this, rec, doc, binaryVersion);
                body = allocedBody;
            }

            // Now save:
            RecordLite newRec;
            newRec.key = doc.docID();
            newRec.flags = doc.flags();
            newRec.body = body;
            newRec.extra = extra;
            newRec.version = binaryVersion;
            newRec.sequence = doc.sequence();
            newRec.updateSequence = false;
            //TODO: Find conflicts and add them to newRec.extra
            defaultKeyStore().set(newRec, transaction);

            ++docCount;
            LogToAt(DBLog, Verbose, "  - Upgraded doc '%.*s', %s -> [%s], %zu bytes body, %zu bytes extra",
                    SPLAT(rec.key()),
                    revid(rec.version()).str().c_str(),
                    string(vv.asASCII()).c_str(),
                    newRec.body.size, newRec.extra.size);
        }

        LogTo(DBLog, "*** %llu documents upgraded, now committing changes... ***", docCount);
    }


    static pair<alloc_slice, alloc_slice> upgradeRemoteRevs(Database *db,
                                                            Record rec,
                                                            RevTreeRecord &doc,
                                                            alloc_slice currentVersion)
    {
        // Instantiate a NuDocument for this document (without reading the database):
        auto currentRev = doc.currentRevision();
        rec.setBody(currentRev->body());
        rec.setVersion(currentVersion);
        
        NuDocument nuDoc(db->defaultKeyStore(), Versioning::RevTrees, rec);
        nuDoc.setEncoder(db->sharedFLEncoder());

        // Add each remote revision:
        for (auto i = doc.remoteRevisions().begin(); i != doc.remoteRevisions().end(); ++i) {
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
