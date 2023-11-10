//
// VersionVectorWithLegacy.hh
//
// Copyright © 2023 Couchbase. All rights reserved.
//

#pragma once
#include "VersionVector.hh"
#include "VectorRecord.hh"
#include "Error.hh"
#include <iostream>

namespace litecore {

    /** A version vector plus a legacy tree-based revid.
        Either or both may be missing.
        If both are present, the legacy revid is interpreted as older than the version vector.
        @note Yes, this class is a kludge. */
    struct VersionVecWithLegacy {
        VersionVector vector;
        revid         legacy;

        /// Constructs from the history array of a C4DocPutRequest.
        VersionVecWithLegacy(const slice history[], size_t historyCount, SourceID mySourceID) {
            parse(history, historyCount, mySourceID);
        }

        VersionVecWithLegacy(std::initializer_list<slice> history, SourceID mySourceID) {
            parse(history.begin(), history.size(), mySourceID);
        }

        void parse(const slice history[], size_t historyCount, SourceID mySourceID) {
            Assert(historyCount > 0);
            if ( revidBuffer lastHistory(history[historyCount - 1]); !lastHistory.getRevID().isVersion() ) {
                // The last history item may be a legacy tree-based revid:
                _legacyBuf = lastHistory;
                legacy     = _legacyBuf.getRevID();
                --historyCount;
            }
            if ( historyCount == 1 ) {
                vector.readASCII(history[0], mySourceID);  // -> Single vector (or single version)
            } else if ( historyCount == 2 ) {
                vector.readASCII(history[1], mySourceID);
                vector.add(Version(history[0]));  // -> New version plus parent vector
            } else if ( historyCount > 2 ) {
                for ( ssize_t i = historyCount - 1; i >= 0; --i )
                    vector.add(Version(history[i]));  // -> List of versions
            }
        }

        /// Constructs from the current revision of a VectorRecord.
        explicit VersionVecWithLegacy(VectorRecord const& rec) {
            if ( revid curRevID = rec.revID(); curRevID && curRevID.isVersion() ) {
                vector = curRevID.asVersionVector();
                legacy = rec.lastLegacyRevID();
            } else {
                legacy = curRevID;
            }
        }

        /// Compares two VersionVecWithLegacy objects.
        static versionOrder compare(VersionVecWithLegacy const& newRev, VersionVecWithLegacy const& curDoc) {
            if ( curDoc.vector.empty() ) {
                // Doc has no version vector:
                if ( !curDoc.legacy ) return kNewer;  // -> doc is new/empty
                else if ( newRev.legacy != curDoc.legacy )
                    return kConflicting;  // -> rq's legacy revid differs, or it doesn't have one
                else if ( newRev.vector.empty() )
                    return kSame;  // -> no-op since neither of us has a version vector
                else
                    return kNewer;  // -> rq has newer vector-based revisions
            } else {
                // Doc has version vector already:
                if ( newRev.legacy && curDoc.legacy && newRev.legacy != curDoc.legacy )
                    return kConflicting;  // -> legacy revids exist but mismatch
                else if ( newRev.vector.empty() )
                    return kConflicting;  // -> rq has only legacy revid
                else
                    return newRev.vector.compareTo(curDoc.vector);  // -> compare the 2 vectors
            }
        }

        friend std::ostream& operator<<(std::ostream& out, VersionVecWithLegacy const& ver) {
            out << ver.vector.asString();
            if ( ver.legacy ) {
                if ( !ver.vector.empty() ) {
                    if ( ver.vector.currentVersions() == ver.vector.count() ) out << "; ";
                    else
                        out << ", ";
                }
                out << ver.legacy.str();
            }
            return out;
        }

      private:
        revidBuffer _legacyBuf;  // backing store for legacy, if needed
    };

}  // namespace litecore
