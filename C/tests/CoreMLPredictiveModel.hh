//
// CoreMLPredictiveModel.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
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
}

namespace cbl {

    /** An abstract adapter class for LiteCore predictive model implementations. */
    class PredictiveModel {
    public:
        PredictiveModel()                                           { }
        virtual ~PredictiveModel()                                  {unregister();}

        void registerWithName(const char* C4NONNULL name);
        void unregister();

    protected:
        /** Subclass must implement this to perform the work. */
        virtual bool predict(fleece::Dict input, fleece::Encoder&, C4Error *error) =0;

        static void encodeMLFeature(fleece::Encoder&, MLFeatureValue*);
        static bool reportError(C4Error *outError, const char *format, ...);

    private:
        PredictiveModel(PredictiveModel&) =delete;
        PredictiveModel& operator=(const PredictiveModel&) =delete;

        std::string _name;
    };


    /** An adapter class that registers a CoreML model with LiteCore for predictive queries. */
    class API_AVAILABLE(macos(10.13), ios(11))
    CoreMLPredictiveModel : public PredictiveModel {
    public:
        CoreMLPredictiveModel(MLModel* C4NONNULL model);

    protected:
        virtual bool predict(fleece::Dict input, fleece::Encoder&, C4Error *error) override;
        
        bool predictViaCoreML(fleece::Dict input, fleece::Encoder&, C4Error *error);
        bool predictViaVision(fleece::Dict input, fleece::Encoder&, C4Error *error);

    private:
        MLFeatureValue* featureFromDict(NSString* name, FLValue, C4Error *outError);
        NSDictionary* convertWordsToMLDictionary(NSString*);

        MLModel* const _model;
        NSDictionary<NSString*,MLFeatureDescription*>* const _featureDescriptions;
        NSLinguisticTagger* _tagger {nil};

        fleece::alloc_slice _imagePropertyName;
        VNCoreMLModel* _visionModel;

        static constexpr unsigned kMaxClassifications {5};
        static constexpr double kConfidenceCutoffRatio = 0.1;
    };

}

#endif
