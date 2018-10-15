//
// PredictiveModel.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "fleece/slice.hh"
#include "Value.hh"
#include <string>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore {

    class PredictiveModel : public fleece::RefCounted {
    public:
        virtual fleece::alloc_slice predict(const fleece::impl::Dict* NONNULL,
                                            C4Error* NONNULL) noexcept =0;

        void registerAs(const std::string &name);
        static bool unregister(const std::string &name);

        static fleece::Retained<PredictiveModel> named(const std::string&);
    };

}

#endif
