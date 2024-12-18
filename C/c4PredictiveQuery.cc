//
// c4PredictiveQuery.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "c4PredictiveQuery.h"
#include "c4Log.h"

#ifdef COUCHBASE_ENTERPRISE

#    include "c4Database.hh"  // IWYU pragma: keep - We need the definition of C4Database for the dynamic cast
#    include "PredictiveModel.hh"

using namespace litecore;
using namespace fleece;
using namespace fleece::impl;

class C4PredictiveModelInternal : public PredictiveModel {
  public:
    explicit C4PredictiveModelInternal(const C4PredictiveModel& model) : _c4Model(model) {}

    alloc_slice prediction(const Dict* input, DataFile::Delegate* dfDelegate, C4Error* outError) noexcept override {
        try {
            return _c4Model.prediction(_c4Model.context, (FLDict)input, dynamic_cast<C4Database*>(dfDelegate),
                                       outError);
        } catch ( const std::exception& x ) {
            if ( outError ) *outError = c4error_make(LiteCoreDomain, kC4ErrorUnexpectedError, slice(x.what()));
            return C4SliceResult{};
        }
    }

  protected:
    ~C4PredictiveModelInternal() override {
        if ( _c4Model.unregistered ) _c4Model.unregistered(_c4Model.context);
    }

  private:
    C4PredictiveModel _c4Model;
};

#endif  // COUCHBASE_ENTERPRISE


void c4pred_registerModel(const char* name, C4PredictiveModel model) C4API {
#ifdef COUCHBASE_ENTERPRISE
    auto context = retained(new C4PredictiveModelInternal(model));
    context->registerAs(name);
#else
    C4WarnError("c4pred_registerModel() is not implemented; aborting");
    abort();
#endif
}

bool c4pred_unregisterModel(const char* name) C4API {
#ifdef COUCHBASE_ENTERPRISE
    return PredictiveModel::unregister(name);
#else
    C4WarnError("c4pred_unregisterModel() is not implemented; aborting");
    abort();
#endif
}
