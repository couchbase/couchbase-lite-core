//
// CoreMLPredictiveModel.mm
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

#include "CoreMLPredictiveModel.hh"
#include "c4PredictiveQuery.h"
#include "fleece/Fleece.hh"
#include <CoreML/CoreML.h>
#include <stdarg.h>

namespace cbl {
    using namespace std;
    using namespace fleece;


    static void reportError(C4Error *outError, const char *format, ...) {
        va_list args;
        va_start(args, format);
        char *message = nullptr;
        vasprintf(&message, format, args);
        va_end(args);
        C4LogToAt(kC4QueryLog, kC4LogError, "prediction() failed: %s", message);
        if (outError)
            *outError = c4error_make(LiteCoreDomain, kC4ErrorInvalidQuery, slice(message));
        free(message);
    }


    CoreMLPredictiveModel::CoreMLPredictiveModel(MLModel *model)
    :_model(model)
    ,_featureDescriptions(_model.modelDescription.inputDescriptionsByName)
    { }


    void CoreMLPredictiveModel::registerWithName(const char *name) {
        _name = name;
        c4pred_registerModel(name, {this, &CoreMLPredictiveModel::predictCallback});
    }


    void CoreMLPredictiveModel::unregister() {
        if (!_name.empty()) {
            c4pred_unregisterModel(_name.c_str());
            _name = "";
        }
    }


    C4SliceResult CoreMLPredictiveModel::predictCallback(void* modelInternal,
                                                         FLValue input,
                                                         C4Error *outError)
    {
        auto self = (CoreMLPredictiveModel*)modelInternal;
        return self->predict(input, outError);
    }


    C4SliceResult CoreMLPredictiveModel::predict(FLValue input, C4Error *outError) {
        // Convert the input dictionary into an MLFeatureProvider:
        Dict inputDict = Value(input).asDict();
        auto featureDict = [NSMutableDictionary new];
        for (NSString *name in _featureDescriptions) {
            Value value = Dict(inputDict)[nsstring_slice(name)];
            if (value) {
                MLFeatureValue *feature = featureFromDict(name, value, outError);
                if (!feature)
                    return {};
                featureDict[name] = feature;
            } else if (!_featureDescriptions[name].optional) {
                reportError(outError, "required input property '%s' is missing", name.UTF8String);
                return {};
            }
        }
        NSError *error;
        auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary: featureDict
                                                                          error: &error];
        NSCAssert(features, @"Failed to create MLDictionaryFeatureProvider");

        // Run the model!
        id<MLFeatureProvider> result = [_model predictionFromFeatures: features error: &error];
        if (!result) {
            const char *msg = error.localizedDescription.UTF8String;
            reportError(outError, "CoreML error: %s", msg);
            return {};
        }

        // Decode the result to Fleece:
        Encoder enc;
        enc.beginDict(result.featureNames.count);
        for (NSString* name in result.featureNames) {
            enc.writeKey(nsstring_slice(name));
            auto feature = [result featureValueForName: name];
            switch (feature.type) {
                case MLFeatureTypeInt64:
                    enc.writeInt(feature.int64Value); break;
                case MLFeatureTypeDouble:
                    enc.writeDouble(feature.doubleValue); break;
                case MLFeatureTypeString:
                    enc.writeString(nsstring_slice(feature.stringValue)); break;
                default:
                    C4Warn("predict(): Don't know how to convert result feature type %ld", //TODO
                           (long)feature.type);
                    enc.writeNull();
            }
        }
        enc.endDict();
        return C4SliceResult(enc.finish());
    }


    // Creates an MLFeatureValue from the value of the same name in a Fleece dictionary.
    MLFeatureValue* CoreMLPredictiveModel::featureFromDict(NSString* name,
                                                           FLValue flValue,
                                                           C4Error *outError)
    {
        Value value(flValue);
        MLFeatureDescription *desc = _featureDescriptions[name];
        auto valueType = value.type();
        MLFeatureValue* feature = nil;
        switch (desc.type) {
            case MLFeatureTypeInt64:
                if (valueType == kFLNumber || valueType == kFLBoolean)
                    feature = [MLFeatureValue featureValueWithInt64: value.asInt()];
                break;
            case MLFeatureTypeDouble:
                if (valueType == kFLNumber)
                    feature = [MLFeatureValue featureValueWithDouble: value.asDouble()];
                break;
            case MLFeatureTypeString: {
                slice str = value.asString();
                if (str)
                    feature = [MLFeatureValue featureValueWithString: str.asNSString()];
                break;
            }
            case MLFeatureTypeImage:
            case MLFeatureTypeMultiArray:
            case MLFeatureTypeDictionary:
            case MLFeatureTypeSequence:
            case MLFeatureTypeInvalid:
                break;
        }
        if (!feature) {
            reportError(outError, "%sinput property '%s' has wrong type",
                        (desc.optional ? "" : "required "),
                        name.UTF8String);
        } else if (![desc isAllowedValue: feature]) {
            reportError(outError, "%sinput property '%s' has an invalid value",
                        (desc.optional ? "" : "required "),
                        name.UTF8String);
            feature = nil;
        }
        return feature;
    }

}
