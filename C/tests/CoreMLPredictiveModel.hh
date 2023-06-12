//
// CoreMLPredictiveModel.hh
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
#include "c4PredictiveQuery.h"
#include "fleece/slice.hh"
#include <Foundation/Foundation.h>
#include <string>
@class MLModel, MLFeatureDescription, MLFeatureValue, NSLinguisticTagger, VNCoreMLModel;

#ifdef COUCHBASE_ENTERPRISE

namespace fleece {
    class Dict;
    class Encoder;
}  // namespace fleece

namespace cbl {

    /** An abstract adapter class for LiteCore predictive model implementations. */
    class PredictiveModel {
      public:
        PredictiveModel()                                  = default;
        PredictiveModel(PredictiveModel&)                  = delete;
        PredictiveModel& operator=(const PredictiveModel&) = delete;

        virtual ~PredictiveModel() { unregister(); }

        /** Registers this instance under the given name. */
        void registerWithName(const char* name);

        /** Unregisters this instance. */
        void unregister();

      protected:
        /** Subclass must implement this to perform the work. */
        virtual bool predict(fleece::Dict input, C4Database* db, fleece::Encoder&, C4Error* error) = 0;

        static void encodeMLFeature(fleece::Encoder&, MLFeatureValue*);
        static bool __printflike(2, 3) reportError(C4Error* outError, const char* format, ...);

      private:
        std::string _name;
    };

    /** An adapter class that registers a CoreML model with LiteCore for predictive queries.
        (Only available on Apple platforms, obviously.) */
    class API_AVAILABLE(ios(11)) CoreMLPredictiveModel : public PredictiveModel {
      public:
        explicit CoreMLPredictiveModel(MLModel* model);

      protected:
        bool predict(fleece::Dict input, C4Database*, fleece::Encoder&, C4Error* error) override;

      private:
        bool            predictViaCoreML(fleece::Dict input, fleece::Encoder&, C4Error* error);
        NSArray*        runVisionFunction(fleece::Dict input, C4Database* db, C4Error* outError);
        bool            decodeVisionResults(NSArray* visionResults, fleece::Encoder&, C4Error* error);
        MLFeatureValue* featureFromDict(NSString* name, FLValue, C4Error* outError);
        NSDictionary*   convertWordsToMLDictionary(NSString*);

        MLModel* const                                        _model;
        NSDictionary<NSString*, MLFeatureDescription*>* const _featureDescriptions;
        NSLinguisticTagger*                                   _tagger{nil};

        fleece::alloc_slice _imagePropertyName;
        VNCoreMLModel*      _visionModel{nil};

        static constexpr unsigned kMaxClassifications{5};
        static constexpr double   kConfidenceCutoffRatio = 0.1;
    };

}  // namespace cbl

#endif
