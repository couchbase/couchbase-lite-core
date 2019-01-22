#ifdef COUCHBASE_ENTERPRISE

//
// PredictiveModel.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
//  COUCHBASE LITE ENTERPRISE EDITION
//
//  Licensed under the Couchbase License Agreement (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//  https://info.couchbase.com/rs/302-GJY-034/images/2017-10-30_License_Agreement.pdf
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "PredictiveModel.hh"
#include <mutex>
#include <unordered_map>

namespace litecore {
    using namespace std;
    using namespace fleece;

    // HACK: Making this a pointer to avoid the dynamic atexit destructor
    // Since the "unregister" callback potentially calls into managed code
    // (i.e. C#, etc) it will cause errors if the runtime has already been
    // unloaded (which is the case during atexit)
    static unordered_map<string, Retained<PredictiveModel>> *sRegistry 
        = new unordered_map<string, Retained<PredictiveModel>>;
    static mutex sRegistryMutex;

    void PredictiveModel::registerAs(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        sRegistry->erase(name);
        sRegistry->insert({name, this});
    }

    bool PredictiveModel::unregister(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        return sRegistry->erase(name) > 0;
    }

    Retained<PredictiveModel> PredictiveModel::named(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        auto i = sRegistry->find(name);
        if (i == sRegistry->end())
            return nullptr;
        return i->second;
    }

}

#endif
