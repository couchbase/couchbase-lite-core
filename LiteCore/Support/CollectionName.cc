//
// CollectionName.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "CollectionName.hh"
#include "Error.hh"
#include "KeyStore.hh"
#include "fleece/slice.hh"

namespace litecore {
    using namespace std;
    using namespace fleece;


    const CollectionName CollectionName::kDefault(string(kC4DefaultCollectionName));

    static bool isValidCollectionName(slice name) {
        return name == kC4DefaultCollectionName || KeyStore::isValidCollectionName(name);
    }

    bool CollectionName::isValid(const C4CollectionSpec& spec) noexcept {
        return isValidCollectionName(spec.name) && (!spec.scope || isValidCollectionName(spec.scope));
    }

    static string mkKeyspace(C4CollectionSpec const& spec) {
        slice name = spec.name ? spec.name : kC4DefaultCollectionName;
        if ( spec.scope == kC4DefaultScopeID || spec.scope.empty() ) return string(name);
        else
            return string(spec.scope).append(".").append(name);
    }

    static C4CollectionSpec mkSpec(string_view keyspace) {
        C4CollectionSpec spec;
        if ( slice ks{keyspace}; auto dot = ks.findByte('.') ) {
            spec.scope = ks.upTo(dot);
            spec.name  = ks.from(dot + 1);
        } else {
            spec.scope = kC4DefaultScopeID;
            spec.name  = ks;
        }
        return spec;
    }

    CollectionName::CollectionName(C4CollectionSpec const& spec) : CollectionName(mkKeyspace(spec)) {}

    CollectionName::CollectionName(string keyspace) : _keyspace(std::move(keyspace)), _spec(mkSpec(_keyspace)) {
        if ( !isValid(_spec) )
            error::_throw(error::InvalidParameter, "Invalid scope/collection name %s", _keyspace.c_str());
    }

    CollectionName::CollectionName(const CollectionName& cn) : _keyspace(cn._keyspace), _spec(mkSpec(_keyspace)) {}

    CollectionName& CollectionName::operator=(const CollectionName& cn) {
        _keyspace = cn._keyspace;
        _spec     = mkSpec(_keyspace);
        return *this;
    }

    bool CollectionName::operator==(C4CollectionSpec const& other) const {
        slice otherScope = other.scope.buf ? other.scope : kC4DefaultScopeID;
        return _spec.name == other.name && _spec.scope == otherScope;
    }

    bool operator<(const CollectionName& spec1, const CollectionName& spec2) noexcept {
        if ( spec1.scope() == spec2.scope() ) {
            if ( spec1.name() == kC4DefaultCollectionName )  // `_default` sorts before anything else
                return (spec1.name() != spec2.name());
            return spec1.name().caseEquivalentCompare(spec2.name()) < 0;
        } else {
            if ( spec1.scope() == kC4DefaultScopeID ) return (spec1.scope() != spec2.scope());
            return spec1.scope().caseEquivalentCompare(spec2.scope()) < 0;
        }
    }

}  // namespace litecore
