#ifdef COUCHBASE_ENTERPRISE

//
// PredictiveModel.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#    include "PredictiveModel.hh"
#    include <mutex>
#    include <unordered_map>

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
        auto              i = sRegistry->find(name);
        if ( i == sRegistry->end() ) return nullptr;
        return i->second;
    }

}  // namespace litecore

#endif
