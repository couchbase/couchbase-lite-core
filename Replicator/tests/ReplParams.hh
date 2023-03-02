//
// Created by Callum Birks on 24/01/2023.
//
/**
 * This class is a wrapper around C4ReplicatorParameters that provides some high-level functions
 * to make it easier to set up replication for testing with Sync Gateway
 */

#pragma once

#include <fleece/Fleece.hh>
using namespace fleece; // Avoid error in ReplicatorOptions
#include "c4ReplicatorTypes.h"
#include "ReplicatorOptions.hh"
#include <vector>
#include <functional>
#include <unordered_map>

using namespace litecore;

using StatusCallback = C4ReplicatorStatusChangedCallback;
using DocsEndedCallback = C4ReplicatorDocumentsEndedCallback;
using BlobProgressCallback = C4ReplicatorBlobProgressCallback;
using EncryptionCallback = C4ReplicatorPropertyEncryptionCallback;
using DecryptionCallback = C4ReplicatorPropertyDecryptionCallback;
using ValidationFunction = C4ReplicatorValidationFunction;

class ReplParams : public C4ReplicatorParameters {
public:
    explicit ReplParams(C4ReplicatorMode push = kC4Disabled, C4ReplicatorMode pull = kC4Disabled);

    fleece::Value getOption(slice key) { return AllocedDict(optionsDictFleece).get(key); }
#pragma mark - SETTERS
    // Set the value of an option in the dict
    template <class T>
    ReplParams& setOption(fleece::slice key, T val) {
        _optionsDict = updateProperties(_optionsDict, key, val);
        optionsDictFleece = _optionsDict.data();
        return *this;
    }
    // Set the value of multiple options in the dict
    ReplParams& setOptions(const AllocedDict& options);
    // Set docID filter for replication
    ReplParams& setDocIDs(const std::unordered_map<alloc_slice, unsigned>& docIDs);
    // Same as above, with array parameter
    template <size_t N>
    ReplParams& setDocIDs(const std::array<std::unordered_map<alloc_slice, unsigned>, N>& docIDs) {
        return setDocIDs({ docIDs.begin(), docIDs.end() });
    }
    // Clear the docID filter
    void clearDocIDs() {
        setDocIDs({});
    }
    // Set the push and pull setting for every collection
    ReplParams& setPushPull(C4ReplicatorMode push, C4ReplicatorMode pull);
    // Set the callback context for a collection
    ReplParams& setCollectionContext(int collectionIndex, void *callbackContext);
    // Set the push filter for collections, ensure you have set collection context first
    ReplParams& setPushFilter(ValidationFunction pushFilter);
    // Set the pull filter for collections, ensure you have set collection context first
    ReplParams& setPullFilter(ValidationFunction pullFilter);

    ReplParams& setStatusCallback(StatusCallback statusCallback);
    ReplParams& setDocsEndedCallback(DocsEndedCallback docsEndedCallback);
    ReplParams& setBlobProgressCallback(BlobProgressCallback blobProgressCallback);
    ReplParams& setPropertyEncryptor(EncryptionCallback encryptionCallback);
    ReplParams& setPropertyDecryptor(DecryptionCallback decryptionCallback);
    ReplParams& setCallbackContext(void *callbackContext);
    ReplParams& setSocketFactory(C4SocketFactory* socketFactory);

    // Overwrite values in params which are non-null in this object
    C4ReplicatorParameters applyThisTo(C4ReplicatorParameters params);
    // Returns C4ParamsSetter, for use in ReplicatorAPITest::replicate()
    std::function<void(C4ReplicatorParameters&)> paramSetter();

private:
    // Retaining dict memory for myself
    AllocedDict _optionsDict;
    // Retain memory for c4ParamsSetters that are created from this object
    std::vector<AllocedDict> _paramSetterOptions;
    // Set the value of multiple options in the dict
    static inline AllocedDict setOptions(const AllocedDict& params, const AllocedDict& options);

    template <class T>
    static fleece::AllocedDict updateProperties(const fleece::AllocedDict& properties, fleece::slice name, T value) {
      fleece::Encoder enc;
      enc.beginDict();
      if (std::is_same<decltype(value), bool>::value) {
        enc.writeKey(name);
        enc.writeBool((bool)value);
      } else if (std::is_arithmetic<decltype(value)>::value || value) {
        enc.writeKey(name);
        enc << value;
      }
      for (fleece::Dict::iterator i(properties); i; ++i) {
        fleece::slice key = i.keyString();
        if (key != name) {
          enc.writeKey(key);
          enc.writeValue(i.value());
        }
      }
      enc.endDict();
      return fleece::AllocedDict(enc.finish());
    }
};