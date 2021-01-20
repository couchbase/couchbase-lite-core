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
#include "RecordEnumerator.hh"
#include "RevID.hh"
#include "VersionedDocument.hh"
#include "VersionVector.hh"
#include "StringUtil.hh"
#include <vector>

namespace c4Internal {
    using namespace litecore;

    // The fake peer/source ID used for versions migrated from revIDs.
    static constexpr peerID kLegacyPeerID {0x7777777};


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
            // Read the doc as a rev-tree based VersionedDocument:
            const Record &rec = e.record();
            VersionedDocument doc(defaultKeyStore(), rec);
            auto currentRev = doc.currentRevision();

            // Make up a version:
            VersionVector vv;
            vv.add({currentRev->revID.generation(), kLegacyPeerID});
            //TODO: If there is an earlier server rev, use it as the base version
            auto binaryVersion = vv.asBinary();

            // Now save:
            RecordLite newRec;
            newRec.key = doc.docID();
            newRec.flags = doc.flags();
            newRec.body = currentRev->body();
            newRec.version = binaryVersion;
            newRec.sequence = doc.sequence();
            newRec.updateSequence = false;
            //TODO: Find conflicts and add them to newRec.extra
            defaultKeyStore().set(newRec, transaction);

            ++docCount;
            LogToAt(DBLog, Verbose, "   Upgraded doc '%.*s', %s -> [%s]",
                    SPLAT(rec.key()),
                    revid(rec.version()).str().c_str(),
                    string(vv.asASCII()).c_str());
        }

        LogTo(DBLog, "*** %llu documents upgraded, now committing changes... ***", docCount);
    }


}
