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

#pragma once
#include "c4DatabaseTypes.h"
#include <string>

namespace litecore {

    /** A memory-safe wrapper around C4CollectionSpec, with backing store for the strings.
        Also provides easy conversion to/from keyspace syntax, e.g. "scope.collection". */
    class CollectionName {
      public:
        static const CollectionName kDefault;

        CollectionName(C4CollectionSpec const&);

        /// Constructor takes a 'keyspace' string of the form "collection" or "scope.collection".
        explicit CollectionName(std::string keyspace);

        CollectionName(const CollectionName& cn);
        CollectionName& operator=(const CollectionName&);

        fleece::slice scope() const { return _spec.scope; }

        fleece::slice name() const { return _spec.name; }

        operator C4CollectionSpec() const noexcept { return _spec; }

        /// Returns a 'keyspace' string of the form "collection" or "scope.collection".
        std::string const& keyspace() const noexcept { return _keyspace; }

        /// Utility that checks a collection spec for validity.
        static bool isValid(const C4CollectionSpec&) noexcept;

        friend bool operator<(const CollectionName&, const CollectionName&) noexcept;

        bool operator==(C4CollectionSpec const&) const;

      private:
        std::string      _keyspace;
        C4CollectionSpec _spec;
    };

}  // namespace litecore
