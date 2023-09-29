//
//  VersionVector.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright 2016-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.

#include "VersionVector.hh"
#include "HybridClock.hh"
#include "Error.hh"
#include "varint.hh"
#include "slice_stream.hh"
#include "NumConversion.hh"
#include <algorithm>

namespace litecore {
    using namespace std;
    using namespace fleece;

    using vec = VersionVector::vec;


#pragma mark - CONVERSION:

    void VersionVector::validate() const {
        if ( empty() ) Assert(_nCurrent == 0);
        else
            Assert(_nCurrent >= 1);
        // Look for illegal duplicate authors:    //OPT: This is O(n^2)
        if ( count() > 1 ) {
            bool dup = false;
            for ( auto i = _vers.begin(); i != _vers.end(); ++i ) {
                SourceID author = i->author();
                for ( auto j = next(i); j != _vers.end(); ++j ) {
                    if ( j->author() == author ) {
                        if ( i == _vers.begin() && (j - _vers.begin()) < _nCurrent && !dup ) {
                            // It's OK for the current author to appear in the merge list (once).
                            // But the current timestamp must be newer than the merge one.
                            dup = true;
                            if ( i->time() <= j->time() ) {
                                error::_throw(error::BadRevisionID, "Cur version not newer than merge version: %s",
                                              asString().c_str());
                            }
                        } else {
                            error::_throw(error::BadRevisionID, "Duplicate ID in version vector: %s",
                                          asString().c_str());
                        }
                    }
                }
            }
        }
    }

    /*  BINARY VERSION VECTOR ENCODING:

        The first byte is always 00, to distinguish this from a binary digest-based revid.
        After that are the Versions, in order.
        Each version consists of its timestamp followed by its SourceID.
        - The first Version's timestamp is written as-is, in logicalTime encoding (see HybridClock.cc)
        - Each following timestamp is written as the difference from the previous one,
          as a signed SVarInt.
        - Each SourceID is encoded as a UVarInt.

        The timestamp encoding saves space because `logicalTime` values are very large integers,
        61 bits or more, but the differences between them are smaller.
     */

    // (These are like the compress/decompress functions in Version.cc, but with signed arithmetic.)
    static int64_t compress(int64_t i) {
        if ( i & 0xFFFF ) return (2 * i) | 1;
        else
            return i / 0x8000;
    }

    static int64_t decompress(int64_t i) {
        if ( i & 1 ) i >>= 1;  // If LSB is set, just remove it
        else
            i *= 0x8000;  // else add 15 more 0 bits
        return i;
    }

    void VersionVector::readBinary(slice data) {
        clear();
        slice_istream in(data);
        if ( in.size < 1 || in.readByte() != 0 ) Version::throwBadBinary();
        uint64_t time = 0;
        int      n    = 0;
        while ( in.size > 0 ) {
            if ( n++ == 0 ) {
                // First timestamp is encoded as-is:
                optional<uint64_t> g = in.readUVarInt();
                if ( !g ) Version::throwBadBinary();
                time = uint64_t(decompress(*g));
            } else {
                // The rest are signed deltas:
                optional<int64_t> g = in.readUVarInt();
                if ( !g ) Version::throwBadBinary();
                time -= decompress(*g);
            }
            // Then the SourceID:
            SourceID id;
            bool     current;
            if ( !id.readBinary(in, &current) ) Version::throwBadBinary();
            _vers.emplace_back(logicalTime{time}, id);
            if ( current ) {
                if ( _nCurrent == n - 1 ) _nCurrent++;
                else
                    Version::throwBadBinary();
            }
        }
        if ( _nCurrent == 0 && !_vers.empty() ) _nCurrent = 1;
        validate();
    }

    alloc_slice VersionVector::asBinary(SourceID myID) const {
        size_t binarySize = 1 + _vers.size() * (kMaxVarintLen64 + 1 + sizeof(SourceID));
        auto   result     = slice_ostream::alloced(binarySize, [&](slice_ostream& out) {
            if ( !out.writeByte(0) )  // leading 0 byte distinguishes it from a `revid`
                return false;
            logicalTime lastTime = logicalTime::none;
            int         n        = 0;
            for ( auto& v : _vers ) {
                bool ok;
                if ( n++ == 0 ) {
                    // First timestamp is encoded as-is:
                    ok = out.writeUVarInt(compress(int64_t(v.time())));
                } else {
                    // The rest are signed deltas:
                    int64_t deltaTime = int64_t(lastTime) - int64_t(v.time());
                    ok                = out.writeUVarInt(compress(deltaTime));
                }
                if ( !ok ) return false;
                lastTime = v.time();

                // Then the SourceID:
                SourceID const& id = v.author().isMe() ? myID : v.author();
                if ( !id.writeBinary(out, n <= _nCurrent) ) return false;
            }
            return true;
        });
        Assert(result);
        return result;
    }

    Version VersionVector::readCurrentVersionFromBinary(slice data) {
        slice_istream in(data);
        if ( data.size < 1 || in.readByte() != 0 ) Version::throwBadBinary();
        return Version(in);
    }

    // The size of the separator is 2. There are size() - 1 separator.
    // Plus, there may be a trailing semi-colon of size 1.
    size_t VersionVector::maxASCIILen() const { return _vers.size() * (Version::kMaxASCIILength + 2); }

    bool VersionVector::writeASCII(slice_ostream& out, SourceID myID) const {
        size_t i = 0;
        for ( auto& v : _vers ) {
            if ( i++ && !out.write(i == _nCurrent + 1 ? "; " : ", ") ) return false;
            if ( !v.writeASCII(out, myID) ) return false;
        }
        if ( _nCurrent > 1 && _nCurrent == count() ) return out.writeByte(';');
        return true;
    }

    alloc_slice VersionVector::asASCII(SourceID myID) const {
        if ( empty() ) return nullslice;
        auto result = slice_ostream::alloced(maxASCIILen(), [&](slice_ostream& out) { return writeASCII(out, myID); });
        Assert(result);
        return result;
    }

    string VersionVector::asString() const { return std::string(asASCII()); }

    void VersionVector::readASCII(slice str, SourceID mySourceID) {
        clear();
        while ( str.size > 0 ) {
            const uint8_t* comma = str.findAnyByteOf(",;");
            if ( !comma ) {
                comma = str.end();
            } else if ( *comma == ';' ) {
                if ( _nCurrent > 0 ) error::_throw(error::BadRevisionID, "multiple ';'s in version vector");
                _nCurrent = _vers.size() + 1;
            }
            _vers.emplace_back(str.upTo(comma), mySourceID);
            str = str.from(comma);
            if ( str.size > 0 ) str.moveStart(1);  // skip comma
            while ( str.hasPrefix(' ') ) str.moveStart(1);
        }
        if ( _nCurrent == 0 && !_vers.empty() ) _nCurrent = 1;
        validate();
    }

    optional<Version> VersionVector::readCurrentVersionFromASCII(slice str) {
        if ( const uint8_t* delim = str.findAnyByteOf(",;") ) str = str.upTo(delim);
        return Version::readASCII(str);
    }

    void VersionVector::readHistory(const slice history[], size_t historyCount, SourceID mySourceID) {
        Assert(historyCount > 0);
        readASCII(history[0], mySourceID);
        if ( historyCount == 1 ) return;  // -> Single version vector (or single version)
        if ( count() > 1 )
            error::_throw(error::BadRevisionID, "Invalid version history (vector followed by other history)");
        if ( historyCount == 2 ) {
            Version newVers = _vers[0];
            readASCII(history[1], mySourceID);
            add(newVers);  // -> New version plus parent vector
        } else {
            for ( size_t i = 1; i < historyCount; ++i ) {
                Version parentVers(history[i], mySourceID);
                if ( auto time = timeOfAuthor(parentVers.author()); time == logicalTime::none )
                    _vers.push_back(parentVers);
                else if ( time <= parentVers.time() )
                    error::_throw(error::BadRevisionID, "Invalid version history (increasing logicalTime)");
            }
        }  // -> List of versions
    }

#pragma mark - OPERATIONS:

    versionOrder VersionVector::compareTo(const Version& v) const {
        auto mine = findPeerIter(v.author());
        if ( mine == _vers.end() || mine->time() < v.time() ) return kOlder;
        else if ( mine->time() == v.time() && mine == _vers.begin() )
            return kSame;
        else
            return kNewer;
    }

    versionOrder Version::compareTo(const VersionVector& vv) const {
        versionOrder o = vv.compareTo(*this);
        if ( o == kOlder ) return kNewer;
        else if ( o == kNewer )
            return kOlder;
        else
            return o;
    }

    versionOrder VersionVector::compareTo(const VersionVector& other) const {
        // First check if either or both are empty:
        auto myCount = count(), otherCount = other.count();
        if ( myCount == 0 ) return otherCount == 0 ? kSame : kOlder;
        else if ( otherCount == 0 )
            return kNewer;

        auto myCmp = this->compareTo(other.current());
        if ( myCmp == kSame ) return kSame;
        auto theirCmp = other.compareTo(this->current());
        Assert(theirCmp != kSame);
        if ( myCmp == theirCmp ) return kConflicting;
        else
            return myCmp;
    }

    bool VersionVector::isNewerIgnoring(SourceID ignoring, const VersionVector& other) const {
        return std::any_of(_vers.begin(), _vers.end(), [&ignoring, other](auto& v) {
            return v.author() != ignoring && v.time() > other[v.author()];
        });
    }

    vec::iterator VersionVector::findPeerIter(SourceID author) const {
        auto& vers = const_cast<VersionVector*>(this)->_vers;
        auto  v    = vers.begin();
        for ( ; v != vers.end(); ++v ) {
            if ( v->author() == author ) break;
        }
        return v;
    }

    logicalTime VersionVector::timeOfAuthor(SourceID author) const {
        auto v = const_cast<VersionVector*>(this)->findPeerIter(author);
        return (v != _vers.end()) ? v->time() : logicalTime::none;
    }

    bool VersionVector::updateClock(HybridClock& clock, bool anyone) const {
        for ( auto& v : _vers )
            if ( !v.updateClock(clock, anyone) ) return false;
        return true;
    }

#pragma mark - MODIFICATION:

    void VersionVector::prune(size_t maxCount, logicalTime before) {
        if ( _vers.size() <= maxCount ) return;
        maxCount = std::max(maxCount, _nCurrent);
        if ( before == logicalTime::endOfTime ) {
            _vers.shrinkTo(maxCount);
        } else {
            for ( auto i = _vers.begin() + maxCount; i != _vers.end(); ) {
                if ( i->time() < before ) i = _vers.erase(i);
                else
                    i++;
            }
        }
#if DEBUG
        validate();
#endif
    }

    bool VersionVector::addNewVersion(HybridClock& clock, SourceID author) {
        if ( auto t = timeOfAuthor(author); t > logicalTime::none )
            if ( !clock.see(t) ) return false;
        return _add(Version(clock.now(), author));
    }

    bool VersionVector::add(Version v) {
        if ( auto t = timeOfAuthor(v.author()); t >= v.time() ) return false;
        return _add(v);
    }

    bool VersionVector::_add(Version const& v) {
        if ( !empty() ) {
            // Remove any existing version(s) by v's author.
            // Also remove any second (merged) version by the current version's author.
            SourceID curAuthor = current().author();
            for ( auto i = _vers.begin(); i != _vers.end(); ) {
                if ( i->author() == v.author() || (i != _vers.begin() && i->author() == curAuthor) ) i = _vers.erase(i);
                else
                    ++i;
            }
        }
        // Now add the new version:
        _vers.insert(_vers.begin(), v);
        _nCurrent = 1;
#if DEBUG
        validate();
#endif
        return true;
    }

    void VersionVector::makeLocal(SourceID myID) {
        if ( !replaceAuthor(myID, kMeSourceID) ) error::_throw(error::BadRevisionID, "Vector already contains '*'");
    }

    void VersionVector::makeAbsolute(SourceID myID) {
        if ( !replaceAuthor(kMeSourceID, myID) ) error::_throw(error::BadRevisionID, "Vector already contains myID");
    }

    bool VersionVector::replaceAuthor(SourceID old, SourceID nuu) noexcept {
        if ( contains(nuu) ) return false;
        for ( Version& v : _vers ) {
            if ( v.author() == old ) v = Version(v.time(), nuu);
        }
        return true;
    }

    bool VersionVector::isAbsolute() const { return !contains(kMeSourceID); }

#pragma mark - MERGING:

    static vec& sortBy(VersionVector::vec& v, bool (*cmp)(Version const&, Version const&)) {
        std::sort(v.begin(), v.end(), cmp);
        return v;
    }

    constexpr SourceID kMaxSourceID(UINT64_MAX, UINT64_MAX);

    VersionVector::vec VersionVector::versionsBySource() const {
        vec sorted = _vers;
        if ( _nCurrent > 1 ) {
            // The current author might appear a second time in a merge revision.
            for ( size_t i = _nCurrent - 1; i > 0; --i ) {
                if ( sorted[i].author() == sorted[0].author() ) {
                    sorted.erase(sorted.begin() + i);
                    break;
                }
            }
        }
        sortBy(sorted, Version::byAuthor);
        sorted.emplace_back(logicalTime(1), kMaxSourceID);  // sentinel to simplify compareBySource
        return sorted;
    }

    bool VersionVector::compareBySource(VersionVector const& v1, VersionVector const& v2, CompareBySourceFn callback) {
        vec  sorted1 = v1.versionsBySource();
        vec  sorted2 = v2.versionsBySource();
        auto i1 = sorted1.begin(), i2 = sorted2.begin();
        bool ok = true;
        do {
            SourceID    author = i1->author();
            logicalTime t1 = i1->time(), t2 = i2->time();
            if ( author == i2->author() ) {
                if ( author == kMaxSourceID ) break;  // reached the end
                // Both have this author:
                i1++;
                i2++;
            } else if ( author < i2->author() ) {
                // i1 has this author but i2 doesn't:
                t2 = logicalTime::none;
                i1++;
            } else {
                // i2 has this author but i1 doesn't:
                author = i2->author();
                t1     = logicalTime::none;
                i2++;
            }
            ok = callback(author, t1, t2);
        } while ( ok );
        return ok;
    }

    VersionVector VersionVector::merge(const VersionVector& v1, const VersionVector& v2, HybridClock& clock) {
        // Start with a new timestamp for me, and the current versions of the two vectors.
        // (Yes, kMeSourceID may occur twice in the vector; it's OK in a merge.)
        if ( !v1.current().updateClock(clock) || !v2.current().updateClock(clock) )
            error::_throw(error::BadRevisionID, "Invalid timestamps in version vector");
        VersionVector result({Version(clock.now(), kMeSourceID), v1.current(), v2.current()}, 3);
        std::sort(result._vers.begin() + 1, result._vers.end(), Version::byDescendingTimes);
        SourceID const& conflictor1 = result[1].author();
        SourceID const& conflictor2 = result[2].author();

        // Walk through the two vectors, adding the most recent timestamp for each other author:
        compareBySource(v1, v2, [&](SourceID author, logicalTime t1, logicalTime t2) {
            // Add the current timestamp of each other author appearing in either vector:
            if ( author != kMeSourceID && author != conflictor1 && author != conflictor2 )
                result._vers.emplace_back(std::max(t1, t2), author);
            return true;
        });

        // Now sort the non-merge versions by descending time, as usual:
        sort(result._vers.begin() + 3, result._vers.end(), Version::byDescendingTimes);
#if DEBUG
        result.validate();
#endif
        return result;
    }

    VersionVector::vec VersionVector::mergedVersions() const {
        if ( _nCurrent <= 1 ) return {};
        vec result(_vers.begin() + 1, _vers.begin() + _nCurrent);
        return sortBy(result, Version::byDescendingTimes);  // Ensure consistent order
    }

    bool VersionVector::mergesSameVersions(VersionVector const& other) const {
        return isMerge() && other.isMerge() && mergedVersions() == other.mergedVersions();
    }

#pragma mark - DELTAS:

    /*  A delta from A to B is a prefix of B,
        containing all the versions in B that are newer than in A. */

    optional<VersionVector> VersionVector::deltaFrom(const VersionVector& src) const {
        if ( src.empty() ) return *this;  // a delta from nothing is the same as me
        else if ( src.count() > count() )
            return nullopt;  // src must be newer if it has more versions; fail

        // Look through myself for a version equal to one in `src`:
        auto i = _vers.begin();
        for ( ; i != _vers.end(); ++i ) {
            logicalTime myTime = i->time(), srcTime = src[i->author()];
            if ( myTime == srcTime ) break;  // found equal version; changes are done
            else if ( myTime < srcTime )
                return nullopt;  // src is newer (or a conflict), so fail
        }
        // Return a prefix of me to before the matching version:
        return VersionVector(_vers.begin(), i);
    }

    VersionVector VersionVector::byApplyingDelta(const VersionVector& delta) const {
        // Reconstruct the target vector by appending to `delta` all components of myself that
        // don't appear in it.
        VersionVector result = delta;
        result._vers.reserve(_vers.size());
        for ( auto& vers : _vers ) {
            if ( auto timeInDelta = delta[vers.author()]; timeInDelta == logicalTime::none )
                result._vers.push_back(vers);
            else if ( timeInDelta < vers.time() )
                error::_throw(error::BadRevisionID, "Invalid VersionVector delta");
        }
        result._nCurrent = !result.empty();
#if DEBUG
        result.validate();
#endif
        assert(result >= *this);  // should be impossible given the above checks
        return result;
    }

}  // namespace litecore
