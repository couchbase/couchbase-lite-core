//
// c4PredictiveQuery.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4PredictiveQuery.h"
#include "PredictiveModel.hh"

using namespace litecore;
using namespace fleece;
using namespace fleece::impl;


class C4PredictiveModelInternal : public PredictiveModel {
public:
    C4PredictiveModelInternal(const C4PredictiveModel &model)
    :_c4Model(model)
    { }

    virtual alloc_slice predict(const Value *input, C4Error *outError) noexcept override {
        return _c4Model.predict(_c4Model.modelInternal, (FLValue)input, outError);
    }

private:
    C4PredictiveModel _c4Model;
};


void c4pred_registerModel(const char *name, C4PredictiveModel model) {
    auto modelInternal = retained(new C4PredictiveModelInternal(model));
    modelInternal->registerAs(name);
}


bool c4pred_unregisterModel(const char *name) {
    return PredictiveModel::unregister(name);
}
