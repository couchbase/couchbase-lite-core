//
// PredictiveModel.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "DataFile.hh"
#include "RefCounted.hh"
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
        virtual fleece::alloc_slice prediction(const fleece::impl::Dict* NONNULL,
                                               DataFile::Delegate* NONNULL,
                                               C4Error* NONNULL) noexcept =0;

        void registerAs(const std::string &name);
        static bool unregister(const std::string &name);

        static fleece::Retained<PredictiveModel> named(const std::string&);
    };

}

#endif
