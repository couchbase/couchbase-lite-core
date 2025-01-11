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
#include <vector>

namespace litecore {

    /** A version vector and/or a legacy tree-based revision history; either or both may be missing.
        If both are present, the legacy history is interpreted as older than the version vector. */
    class VersionVecWithLegacy {
      public:
        /// Constructs from a revid.
        explicit VersionVecWithLegacy(revid revID) {
            if ( revID ) {
                if ( revID.isVersion() ) vector = VersionVector::fromBinary(revID);
                else
                    legacy.emplace_back(revID);
            }
        }

        /// Constructs from a Revision of a VectorRecord.
        explicit VersionVecWithLegacy(VectorRecord const& record, RemoteID remote)
            : VersionVecWithLegacy(record.remoteRevision(remote).value().revID) {
            if ( remote == RemoteID::Local && record.lastLegacyRevID() ) {
                DebugAssert(legacy.empty());
                legacy.emplace_back(record.lastLegacyRevID());
            }
        }

        /// Constructs from the history array of a C4DocPutRequest.
        VersionVecWithLegacy(const slice history[], size_t historyCount, SourceID mySourceID) {
            parse(history, historyCount, mySourceID);
        }

        /// Constructs from an initializer list, for the convenience of unit tests.
        VersionVecWithLegacy(std::initializer_list<slice> history, SourceID mySourceID) {
            parse(history.begin(), history.size(), mySourceID);
        }

        /// The version vector. May be empty.
        VersionVector vector;


        /// The legacy (tree-based) revid history, in reverse chronological order. May be empty.
        /// Must be ordered by descending generation (see `sortLegacy()`.)
        std::vector<alloc_slice> legacy;

        /// Sorts the `legacy` vector into canonical order (by descending generation.)
        void sortLegacy() {
            std::sort(legacy.begin(), legacy.end(),
                      [](auto& r1, auto& r2) { return revid(r1).generation() > revid(r2).generation(); });
        }

        /// Compares two VersionVecWithLegacy objects.
        static versionOrder compare(VersionVecWithLegacy const& a, VersionVecWithLegacy const& b) {
            extendedVersionOrder vectorOrder = extendedCompare(a.vector, b.vector);
            extendedVersionOrder legacyOrder = extendedCompare(a.legacy, b.legacy);
            if ( vectorOrder == kXConflicting || legacyOrder == kXConflicting ) return kConflicting;
            else
                return versionOrder(kVersionOrderMatrix[vectorOrder][legacyOrder]);
        }

        /// Writes an ASCII representation to a stream.
        void write(std::ostream& out, SourceID const& mySourceID) const {
            out << vector.asString(mySourceID);
            if ( !legacy.empty() ) {
                std::string_view delimiter;
                if ( !vector.empty() ) {
                    if ( vector.currentVersions() == vector.count() ) delimiter = "; ";
                    else
                        delimiter = ", ";
                }
                for ( alloc_slice const& rev : legacy ) {
                    out << delimiter << revid(rev).str();
                    delimiter = ", ";
                }
            }
        }

        friend std::ostream& operator<<(std::ostream& out, VersionVecWithLegacy const& ver) {
            ver.write(out, kMeSourceID);
            return out;
        }

      private:
        /// Common code in constructor to initialize from a history array.
        void parse(const slice history[], size_t historyCount, SourceID mySourceID) {
            // The last history item(s) may be legacy tree-based revids:
            while ( historyCount > 0 ) {
                revidBuffer lastHistory(history[historyCount - 1]);
                if ( lastHistory.getRevID().isVersion() ) break;
                legacy.emplace_back(lastHistory.getRevID());
                --historyCount;
            }
            std::reverse(legacy.begin(), legacy.end());

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

        enum extendedVersionOrder : uint8_t {
            kXBothEmpty   = 0,                 // Both are empty
            kXOlderEmpty  = 1,                 // A is older because it's empty, B isn't
            kXNewerEmpty  = 2,                 // A is newer because it's nonempty, B is empty
            kXSame        = 3 + kSame,         // They're equal (both nonempty)
            kXOlder       = 3 + kOlder,        // A is older (both nonempty)
            kXNewer       = 3 + kNewer,        // A is newer (both nonempty)
            kXConflicting = 3 + kConflicting,  // They conflict (both nonempty)
        };

        // Table mapping the extendedVersionOrder of vectors and legacy into a versionOrder.
        // `kTable[vec extOrder][legacy extOrder]` -> versionOrder
        static constexpr uint8_t kVersionOrderMatrix[6][6] = {
                {0, 1, 2, 0, 1, 2},  // both vectors empty
                {1, 1, 3, 1, 1, 3},  // vector a is older because it's empty
                {2, 3, 2, 2, 3, 2},  // vector a is newer because b is empty
                {0, 3, 3, 0, 3, 3},  // vectors equal (nonempty)
                {1, 3, 3, 1, 3, 3},  // vector a is older
                {2, 3, 3, 2, 3, 3},  // vector a is newer
        };

        /// Compares two vectors or legacy histories, also taking into account whether either is empty.
        template <class V>
        static extendedVersionOrder extendedCompare(V const& a, V const& b) {
            versionOrder emptyOrder = mkOrder(!a.empty(), !b.empty());
            if ( emptyOrder != kConflicting ) return extendedVersionOrder(emptyOrder);  // -> one or both is empty
            else
                return extendedVersionOrder(3 + _compare(a, b));  // -> neither empty; do a proper compare
        }

        static versionOrder _compare(VersionVector const& a, VersionVector const& b) { return a.compareTo(b); }

        /// Compares two legacy revision histories. They must be non-empty.
        static versionOrder _compare(std::vector<alloc_slice> const& a, std::vector<alloc_slice> const& b) {
            DebugAssert(!a.empty() && !b.empty());
            for ( auto iNew = a.begin(); iNew != a.end(); ++iNew ) {
                auto iCur = std::find(b.begin(), b.end(), *iNew);
                if ( iCur != b.end() ) {
                    // Found common rev. The history with newer (earlier) revs is newer:
                    return mkOrder(iNew != a.begin(), iCur != b.begin());
                }
            }
            return kConflicting;  // no common revid at all
        }
    };

}  // namespace litecore
