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
#include "c4Base.h"
#include "fleece/slice.hh"
#include <string>

#ifdef COUCHBASE_ENTERPRISE

namespace fleece::impl {
    class Dict;
}

namespace litecore {

    class PredictiveModel : public fleece::RefCounted {
      public:
        virtual fleece::alloc_slice prediction(const fleece::impl::Dict* NONNULL, DataFile::Delegate* NONNULL,
                                               C4Error* NONNULL) noexcept
                = 0;

        void        registerAs(const std::string& name);
        static bool unregister(const std::string& name);

        static fleece::Retained<PredictiveModel> named(const std::string&);
    };

}  // namespace litecore

#endif
