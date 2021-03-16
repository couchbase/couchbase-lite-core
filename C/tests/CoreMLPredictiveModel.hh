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
        PredictiveModel()                                           =default;
        virtual ~PredictiveModel()                                  {unregister();}

        /** Registers this instance under the given name. */
        void registerWithName(const char*name);

        /** Unregisters this instance. */
        void unregister();

    protected:
        /** Subclass must implement this to perform the work. */
        virtual bool predict(fleece::Dict input,
                             C4Database *db,
                             fleece::Encoder&,
                             C4Error *error) =0;

        static void encodeMLFeature(fleece::Encoder&, MLFeatureValue*);
        static bool __printflike(2, 3) reportError(C4Error *outError, const char *format, ...);

    private:
        PredictiveModel(PredictiveModel&) =delete;
        PredictiveModel& operator=(const PredictiveModel&) =delete;

        std::string _name;
    };


    /** An adapter class that registers a CoreML model with LiteCore for predictive queries.
        (Only available on Apple platforms, obviously.) */
    class API_AVAILABLE(macos(10.13), ios(11))
    CoreMLPredictiveModel : public PredictiveModel {
    public:
        CoreMLPredictiveModel(MLModel*model);

    protected:
        virtual bool predict(fleece::Dict input,
                             C4Database*,
                             fleece::Encoder&,
                             C4Error *error) override;
        
    private:
        bool predictViaCoreML(fleece::Dict input, fleece::Encoder&, C4Error *error);
        NSArray* runVisionFunction(fleece::Dict input,
                                   C4Database *db,
                                   C4Error *outError);
        bool decodeVisionResults(NSArray* visionResults, fleece::Encoder&, C4Error *error);
        MLFeatureValue* featureFromDict(NSString* name, FLValue, C4Error *outError);
        NSDictionary* convertWordsToMLDictionary(NSString*);

        MLModel* const _model;
        NSDictionary<NSString*,MLFeatureDescription*>* const _featureDescriptions;
        NSLinguisticTagger* _tagger {nil};

        fleece::alloc_slice _imagePropertyName;
        VNCoreMLModel* _visionModel {nil};

        static constexpr unsigned kMaxClassifications {5};
        static constexpr double kConfidenceCutoffRatio = 0.1;
    };

}

#endif
