//
// CoreMLPredictiveModel.mm
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "CoreMLPredictiveModel.hh"
#include "c4PredictiveQuery.h"
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "fleece/Fleece.hh"
#include "fleece/Fleece+CoreFoundation.h"
#include <CoreML/CoreML.h>
#include <Vision/Vision.h>
#include <stdarg.h>

#ifdef COUCHBASE_ENTERPRISE

#ifdef MAC_OS_X_VERSION_10_14
#define SDK_HAS_SEQUENCES           // CoreML sequence APIs were added in 10.14 / iOS 12 SDK
#endif

namespace cbl {
    using namespace std;
    using namespace fleece;


    void PredictiveModel::registerWithName(const char *name) {
        auto callback = [](void* context, FLDict input, C4Database *db, C4Error *outError) {
            @autoreleasepool {
                Encoder enc;
                enc.beginDict();
                auto self = (PredictiveModel*)context;
                if (!self->predict(Dict(input), db, enc, outError))
                    return C4SliceResult{};
                enc.endDict();
                return C4SliceResult(enc.finish());
            }
        };
        C4PredictiveModel model = {.context = this, .prediction = callback};
        c4pred_registerModel(name, model);
        _name = name;
    }


    void PredictiveModel::unregister() {
        if (!_name.empty()) {
            c4pred_unregisterModel(_name.c_str());
            _name = "";
        }
    }


    // If `outError` is non-null, stores a C4Error in it with the formatted message.
    // Otherwise, it just logs the message at verbose level as a non-fatal issue.
    // Always returns false, as a convenience to the caller.
    bool PredictiveModel::reportError(C4Error *outError, const char *format, ...) {
        if (outError || c4log_getLevel(kC4QueryLog) >= kC4LogVerbose) {
            va_list args;
            va_start(args, format);
            char *message = nullptr;
            if (vasprintf(&message, format, args) < 0)
                throw bad_alloc();
            va_end(args);
            if (outError)
                *outError = c4error_make(LiteCoreDomain, kC4ErrorInvalidQuery, slice(message));
            else
                C4LogToAt(kC4QueryLog, kC4LogVerbose, "prediction() giving up on this row: %s", message);
            free(message);
        }
        return false;
    }


#pragma mark - CoreMLPredictiveModel:


    static NSDictionary* convertToMLDictionary(Dict);


    CoreMLPredictiveModel::CoreMLPredictiveModel(MLModel *model)
    :_model(model)
    ,_featureDescriptions(_model.modelDescription.inputDescriptionsByName)
    {
        // Check for image inputs:
        for (NSString *inputName in _featureDescriptions) {
            if (_featureDescriptions[inputName].type == MLFeatureTypeImage) {
                _imagePropertyName = nsstring_slice(inputName);
                break;
            }
        }
    }


    // The main prediction function!
    bool CoreMLPredictiveModel::predict(Dict inputDict,
                                        C4Database *db,
                                        fleece::Encoder &enc,
                                        C4Error *outError)
    {
        if (_imagePropertyName) {
            if (!_visionModel) {
                NSError* error;
                _visionModel = [VNCoreMLModel modelForMLModel: _model error: &error];
                if (!_visionModel)
                    return reportError(outError, "Failed to create Vision model: %s",
                                       error.localizedDescription.UTF8String);
            }
            NSArray* visionResults = runVisionFunction(inputDict, db, outError);
            return visionResults && decodeVisionResults(visionResults, enc, outError);
        } else {
            return predictViaCoreML(inputDict, enc, outError);
        }
    }


    // Uses CoreML API to generate prediction:
    bool CoreMLPredictiveModel::predictViaCoreML(Dict inputDict, fleece::Encoder &enc, C4Error *outError) {
        // Convert the input dictionary into an MLFeatureProvider:
        auto featureDict = [NSMutableDictionary new];
        for (NSString *name in _featureDescriptions) {
            Value value = Dict(inputDict)[nsstring_slice(name)];
            if (value) {
                MLFeatureValue *feature = featureFromDict(name, value, outError);
                if (!feature)
                    return false;
                featureDict[name] = feature;
            } else if (!_featureDescriptions[name].optional) {
                return reportError(nullptr, "required input property '%s' is missing", name.UTF8String);
            }
        }
        NSError *error;
        auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary: featureDict
                                                                          error: &error];
        NSCAssert(features, @"Failed to create MLDictionaryFeatureProvider");

        // Run the model!
        id<MLFeatureProvider> result = [_model predictionFromFeatures: features error: &error];
        if (!result)
            return reportError(outError, "CoreML error: %s", error.localizedDescription.UTF8String);

        // Decode the result to Fleece:
        for (NSString* name in result.featureNames) {
            enc.writeKey(nsstring_slice(name));
            encodeMLFeature(enc, [result featureValueForName: name]);
        }
        return true;
    }


    NSArray* CoreMLPredictiveModel::runVisionFunction(fleece::Dict input,
                                                      C4Database *db,
                                                      C4Error *outError)
    {
        // Get the image data:
        auto value = input[_imagePropertyName];
        slice image = value.asData();
        alloc_slice allocedImage;
        if (!image) {
            // Input param is not data; assume it's a CBL blob dictionary:
            Dict blobDict = value.asDict();
            if (blobDict) {
                C4Error error;
                auto blobStore = c4db_getBlobStore(db, &error);
                if (!blobStore) {
                    reportError(outError, "Unable to get BlobStore");
                    return nil;
                }
                allocedImage = c4doc_getBlobData(blobDict, blobStore, &error);
                image = allocedImage;
            }
        }
        if (!image) {
            reportError(nullptr, "Image input property '%.*s' missing or not a blob",
                        FMTSLICE(_imagePropertyName));
            return nil;
        }

        @autoreleasepool {
            // Create a Vision handler:
            NSData* imageData = image.uncopiedNSData();
            auto handler = [[VNImageRequestHandler alloc] initWithData: imageData options: @{}];

            // Process the model:
            NSError* error;
            auto request = [[VNCoreMLRequest alloc] initWithModel: _visionModel];
            request.imageCropAndScaleOption = VNImageCropAndScaleOptionCenterCrop;
            if (![handler performRequests: @[request] error: &error]) {
                reportError(outError, "Image processing failed: %s",
                            error.localizedDescription.UTF8String);
                return nil;
            }
            NSArray* results = request.results;
            if (!results)
                reportError(nullptr, "Image processing returned no results");
            return results;
        }
    }


    // Uses Vision API to generate prediction:
    bool CoreMLPredictiveModel::decodeVisionResults(NSArray* results,
                                                 Encoder &enc,
                                                 C4Error *outError) {
        if (!results)
            return false;
        if (!_model)
            return reportError(outError, "Couldn't register Vision model");

        NSString* predictedProbabilitiesName = _model.modelDescription.predictedProbabilitiesName;
        if (predictedProbabilitiesName) {
            // Result is a list of identifiers in declining order of confidence:
            enc.writeKey(nsstring_slice(predictedProbabilitiesName));
            enc.beginDict();
            NSString* maxIdentifier = nil;
            double maxConfidence = 0.0;
            unsigned nClassifications = 0;
            for (VNClassificationObservation* result in results) {
                NSString* identifier = result.identifier;
                double confidence = result.confidence;
                if (!maxIdentifier) {
                    maxIdentifier = identifier;
                    maxConfidence = confidence;
                } else if (confidence < maxConfidence * kConfidenceCutoffRatio) {
                    break;
                }
                enc.writeKey(nsstring_slice(identifier));
                enc.writeDouble(confidence);
                if (++nClassifications >= kMaxClassifications)
                    break;
            }
            enc.endDict();
            NSString* predictedFeatureName = _model.modelDescription.predictedFeatureName;
            if (predictedFeatureName && maxIdentifier) {
                enc.writeKey(nsstring_slice(predictedFeatureName));
                enc.writeString(nsstring_slice(maxIdentifier));
            }
        } else {
            // Result is a list of CoreML feature values:
            for (VNCoreMLFeatureValueObservation* result in results) {
                auto feature = result.featureValue;
                enc.writeKey("output"_sl);      //FIX: How do I find the feature name?
                encodeMLFeature(enc, feature);
            }
        }
        return true;
    }


#pragma mark - INPUT FEATURE CONVERSION:


    static const char* kMLFeatureTypeName[8] = {
        "(invalid)", "int64", "double", "string", "image", "multi-array", "dictionary", "sequence"
    };
    static const char* kCompatibleMLFeatureTypeName[8] = {
        "(invalid)", "numeric", "numeric", "string", "blob", "numeric array", "dictionary", "sequence"
    };


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
            case MLFeatureTypeDictionary: {
                NSDictionary *dict;
                if (valueType == kFLDict) {
                    dict = convertToMLDictionary(value.asDict());
                    if (!dict) {
                        reportError(nullptr, "input dictionary '%s' contains a non-numeric value",
                                    name.UTF8String);
                        return nil;
                    }
                } else if (valueType == kFLString) {
                    // If a string is given where a dictionary is wanted, assume the dictionary is
                    // supposed to map words to counts.
                    dict = convertWordsToMLDictionary(value.asString().asNSString());
                }
                if (dict)
                    feature = [MLFeatureValue featureValueWithDictionary: dict error: nullptr];
                break;
            }
            case MLFeatureTypeImage:        // image features are handled by predictViaVision
            case MLFeatureTypeMultiArray:
#ifdef SDK_HAS_SEQUENCES
            case MLFeatureTypeSequence:
#endif
            case MLFeatureTypeInvalid:
                reportError(outError, "MLModel input feature '%s' is of unsupported type %s; sorry!",
                            name.UTF8String, kMLFeatureTypeName[desc.type]);
                return nil;
            default:
                break;
        }
        if (!feature) {
            reportError(nullptr, "input property '%s' has wrong type; should be %s",
                        name.UTF8String, kCompatibleMLFeatureTypeName[desc.type]);
        } else if (![desc isAllowedValue: feature]) {
            reportError(nullptr, "input property '%s' has an invalid value", name.UTF8String);
            feature = nil;
        }
        return feature;
    }


    // Converts a Fleece dictionary to an NSDictionary. All values must be numeric.
    static NSDictionary* convertToMLDictionary(Dict dict) {
        auto nsdict = [[NSMutableDictionary alloc] initWithCapacity: dict.count()];
        for (Dict::iterator i(dict); i; ++i) {
            // Apparently CoreML dictionary features can only contain numbers...
            if (i.value().type() != kFLNumber)
                return nil;
            nsdict[i.keyString().asNSString()] = @(i.value().asDouble());
        }
        return nsdict;
    }


    // Converts a string into a dictionary that maps its words to the number of times they appear.
    NSDictionary* CoreMLPredictiveModel::convertWordsToMLDictionary(NSString* input) {
        constexpr auto options = NSLinguisticTaggerOmitWhitespace |
                                 NSLinguisticTaggerOmitPunctuation |
                                 NSLinguisticTaggerOmitOther;
        if (!_tagger) {
            auto schemes = [NSLinguisticTagger availableTagSchemesForLanguage: @"en"]; //FIX: L10N
            _tagger = [[NSLinguisticTagger alloc] initWithTagSchemes: schemes options: options];
        }

        auto words = [NSMutableDictionary new];
        _tagger.string = input;
        [_tagger enumerateTagsInRange: NSMakeRange(0, input.length)
                              scheme: NSLinguisticTagSchemeNameType
                             options: options
                          usingBlock: ^(NSLinguisticTag tag, NSRange tokenRange,
                                        NSRange sentenceRange, BOOL *stop)
         {
             if (tokenRange.length >= 3) {  // skip 1- and 2-letter words
                 NSString *token = [input substringWithRange: tokenRange].localizedLowercaseString;
                 NSNumber* count = words[token];
                 words[token] = @(count.intValue + 1);
             }
         }];
        return words;
    }


#pragma mark - OUTPUT FEATURE CONVERSION:


    template <typename T>
    static void encodeInner(Encoder &enc, const T *data, size_t stride, NSUInteger n) {
        for (NSUInteger i = 0; i < n; i++) {
            enc << *data;
            data += stride;
        }
    }


    API_AVAILABLE(macos(10.13), ios(11))
    static void encodeMultiArray(Encoder &enc, MLMultiArray* array,
                                 NSUInteger dimension, const uint8_t *data)
    {
        bool outer = (dimension + 1 < array.shape.count);
        auto n = array.shape[dimension].unsignedIntegerValue;
        auto stride = array.strides[dimension].unsignedIntegerValue;
        auto dataType = array.dataType;
        enc.beginArray();
        if (outer) {
            switch (dataType) {
                case MLMultiArrayDataTypeInt32:     stride *= sizeof(int32_t); break;
                case MLMultiArrayDataTypeFloat32:   stride *= sizeof(float); break;
                case MLMultiArrayDataTypeDouble:    stride *= sizeof(double); break;
                default: NSCAssert(false, @"Unexpected switch case."); break;
            }
            for (NSUInteger i = 0; i < n; i++) {
                encodeMultiArray(enc, array, dimension + 1, data);
                data += stride;
            }
        } else {
            switch (dataType) {
                case MLMultiArrayDataTypeInt32:
                    encodeInner(enc, (const uint32_t*)data, stride, n);
                    break;
                case MLMultiArrayDataTypeFloat32:
                    encodeInner(enc, (const float*)data, stride, n);
                    break;
                case MLMultiArrayDataTypeDouble:
                    encodeInner(enc, (const double*)data, stride, n);
                    break;
                default:
                    NSCAssert(false, @"Unexpected switch case.");
                    break;
            }
        }
        enc.endArray();
    }

    // Encodes a multi-array feature as nested Fleece arrays of numbers.
    API_AVAILABLE(macos(10.13), ios(11))
    static void encodeMultiArray(Encoder &enc, MLMultiArray* array) {
        encodeMultiArray(enc, array, 0, (const uint8_t*)array.dataPointer);
    }


#ifdef SDK_HAS_SEQUENCES
    // Encodes a sequence feature as a Fleece array of strings or numbers.
    API_AVAILABLE(macos(10.14), ios(12.0))
    static void encodeSequence(Encoder &enc, MLSequence *sequence) {
        switch (sequence.type) {
            case MLFeatureTypeString:
                FLEncoder_WriteNSObject(enc, sequence.stringValues); break;
            case MLFeatureTypeInt64:
                FLEncoder_WriteNSObject(enc, sequence.int64Values); break;
            default:
                enc.writeNull(); break;     // MLSequence API doesn't support any other types...
        }
    }
#endif


    // Encodes an ML feature to Fleece.
    API_AVAILABLE(macos(10.13), ios(11))
    void PredictiveModel::encodeMLFeature(Encoder &enc, MLFeatureValue *feature) {
        switch (feature.type) {
            case MLFeatureTypeInt64:
                enc.writeInt(feature.int64Value);
                break;
            case MLFeatureTypeDouble:
                enc.writeDouble(feature.doubleValue);
                break;
            case MLFeatureTypeString:
                enc.writeString(nsstring_slice(feature.stringValue));
                break;
            case MLFeatureTypeDictionary:
                FLEncoder_WriteNSObject(enc, feature.dictionaryValue);
                break;
            case MLFeatureTypeMultiArray:
                encodeMultiArray(enc, feature.multiArrayValue);
                break;
#ifdef SDK_HAS_SEQUENCES
            case MLFeatureTypeSequence:
                if (@available(macOS 10.14, ios 12.0, *))
                    encodeSequence(enc, feature.sequenceValue);
                else
                    enc.writeNull();
                break;
#endif
            case MLFeatureTypeImage:
                C4Warn("predict(): Don't know how to convert result MLFeatureTypeImage");//TODO
                enc.writeNull();
                break;
            case MLFeatureTypeInvalid:
            default:
                enc.writeNull();
                break;
        }
    }
}

#endif // COUCHBASE_ENTERPRISE
