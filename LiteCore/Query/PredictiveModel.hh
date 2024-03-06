//
// PredictiveModel.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "DataFile.hh"
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include <string>

#ifdef COUCHBASE_ENTERPRISE

namespace fleece::impl {
    class Dict;
}

namespace litecore {

    /** Abstract superclass of predictive models. A model consists of a `prediction` function.
        Implemented by C4PredictiveModelInternal, which bridges to the public C4PredictiveModel. */
    class PredictiveModel : public fleece::RefCounted {
      public:
        /// Given a document body, matches it against the model and returns an (encoded) Dict
        /// containing predictive info like ratings, rankings, etc.
        /// This must be a pure function that, given the same input, always produces the
        /// same output; otherwise predictive indexes wouldn't work.
        virtual fleece::alloc_slice prediction(const fleece::impl::Dict* NONNULL, DataFile::Delegate* NONNULL,
                                               C4Error* NONNULL) noexcept = 0;

        /// Registers a model instance globally, with a unique name.
        void registerAs(const std::string& name);

        /// Unregisters the model instance with the given name.
        static bool unregister(const std::string& name);

        /// Returns the instance registered under the given name.
        static fleece::Retained<PredictiveModel> named(const std::string&);
    };

}  // namespace litecore

#endif
