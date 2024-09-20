//
// Created by Callum Birks on 07/11/2022.
//
/**
 * This class is a wrapper around C4ReplicatorParameters that provides some high-level functions
 * to make it easier to set up replication for testing with Sync Gateway
 */

#ifndef LITECORE_REPLPARAMS_HH
#define LITECORE_REPLPARAMS_HH

#include "fleece/Expert.hh"
#include "c4ReplicatorTypes.h"
#include "ReplicatorOptions.hh"
#include <cstdint>
#include <vector>
#include <unordered_map>

using namespace fleece;
using namespace litecore;

using StatusCallback       = C4ReplicatorStatusChangedCallback;
using DocsEndedCallback    = C4ReplicatorDocumentsEndedCallback;
using BlobProgressCallback = C4ReplicatorBlobProgressCallback;
using EncryptionCallback   = C4ReplicatorPropertyEncryptionCallback;
using DecryptionCallback   = C4ReplicatorPropertyDecryptionCallback;
using ValidationFunction   = C4ReplicatorValidationFunction;

class ReplParams : public C4ReplicatorParameters {
  public:
    // Constructor allows for passing all the same objects as C4ReplicatorParameters,
    explicit ReplParams(const std::vector<C4ReplicationCollection>& collections);

    explicit ReplParams(const std::vector<C4CollectionSpec>& collSpecs, C4ReplicatorMode push = kC4Disabled,
                        C4ReplicatorMode pull = kC4Disabled);

    ReplParams(const ReplParams& other);
    // Add collections to the params
    void addCollections(const std::vector<C4ReplicationCollection>& collections);

    // Get the value of an option in the dict
    fleece::Value getOption(slice key) { return AllocedDict(optionsDictFleece).get(key); }

#pragma mark - SETTERS

    // Set the value of an option in the dict
    template <class T>
    ReplParams& setOption(fleece::slice key, T val) {
        _optionsDict      = repl::Options::updateProperties(_optionsDict, key, val);
        optionsDictFleece = _optionsDict.data();
        return *this;
    }

    // Set the value of multiple options in the dict
    ReplParams& setOptions(const AllocedDict& options);
    // Set an option for a single collection
    ReplParams& setCollectionOptions(C4CollectionSpec collectionSpec, const AllocedDict& options);
    // Set an option for all collections
    ReplParams& setCollectionOptions(const AllocedDict& options);
    // Set docIDs in options of each collection
    ReplParams& setDocIDs(const std::vector<std::unordered_map<alloc_slice, uint64_t>>& docIDs);

    // Same as above, with array parameter
    template <size_t N>
    ReplParams& setDocIDs(const std::array<std::unordered_map<alloc_slice, uint64_t>, N>& docIDs) {
        return setDocIDs({docIDs.begin(), docIDs.end()});
    }

    // Clear the docID filter
    void clearDocIDs() { setDocIDs({}); }

    // Set the push and pull setting for every collection
    ReplParams& setPushPull(C4ReplicatorMode push, C4ReplicatorMode pull);
    // Set the callback context for a collection
    ReplParams& setCollectionContext(int collectionIndex, void* callbackContext);
    // Set the push filter for collections, ensure you have set collection context first
    ReplParams& setPushFilter(ValidationFunction pushFilter);
    // Set the pull filter for collections, ensure you have set collection context first
    ReplParams& setPullFilter(ValidationFunction pullFilter);

    ReplParams& setStatusCallback(StatusCallback statusCallback);
    ReplParams& setDocsEndedCallback(DocsEndedCallback docsEndedCallback);
    ReplParams& setBlobProgressCallback(BlobProgressCallback blobProgressCallback);
    ReplParams& setPropertyEncryptor(EncryptionCallback encryptionCallback);
    ReplParams& setPropertyDecryptor(DecryptionCallback decryptionCallback);
    ReplParams& setCallbackContext(void* callbackContext);
    ReplParams& setSocketFactory(C4SocketFactory* socketFactory);

    // Overwrite values in params which are non-null in this object
    C4ReplicatorParameters applyThisTo(C4ReplicatorParameters params);
    // Returns C4ParamsSetter, for use in ReplicatorAPITest::replicate()
    std::function<void(C4ReplicatorParameters&)> paramSetter();

  private:
    // Retain collections memory for myself
    std::vector<C4ReplicationCollection> _collectionVector;
    // Retaining dict memory for myself
    AllocedDict _optionsDict;
    // Retain memory for c4ParamsSetters that are created from this object
    std::vector<AllocedDict> _paramSetterOptions;
    // Retain memory for collections optionsDict
    std::vector<AllocedDict> _collectionsOptionsDict;

    // Set the value of multiple options in the dict
    static AllocedDict setOptions(const AllocedDict& params, const AllocedDict& options);
};

#endif  //LITECORE_REPLPARAMS_HH
