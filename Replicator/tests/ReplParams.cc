//
// Created by Callum Birks on 07/11/2022.
//

#include "ReplParams.hh"

ReplParams::ReplParams(const std::vector<C4ReplicationCollection>& collections_)
    : C4ReplicatorParameters()
{
    _collectionVector = { collections_ };
    collections = _collectionVector.data();
    collectionCount = _collectionVector.size();
    _optionsDict = {};
    optionsDictFleece = _optionsDict.data();
}

ReplParams::ReplParams(const std::vector<C4CollectionSpec> &collSpecs, C4ReplicatorMode push, C4ReplicatorMode pull)
    : C4ReplicatorParameters()
{
    _collectionVector = std::vector<C4ReplicationCollection>{ collSpecs.size() };
    for(int i = 0; i < collSpecs.size(); ++i) {
        _collectionVector[i] = { collSpecs[i], push, pull };
    }
    collections = _collectionVector.data();
    collectionCount = _collectionVector.size();
    _optionsDict = {};
    optionsDictFleece = _optionsDict.data();
}

ReplParams::ReplParams(const ReplParams &other)
    : C4ReplicatorParameters(), _collectionVector(other._collectionVector)
    , _collectionsOptionsDict(other._collectionsOptionsDict), _paramSetterOptions(other._paramSetterOptions)
    , _optionsDict(other._optionsDict)
{
    collections = _collectionVector.data();
    collectionCount = other.collectionCount;
    optionsDictFleece = _optionsDict.data();
    onStatusChanged = other.onStatusChanged;
    onDocumentsEnded = other.onDocumentsEnded;
    onBlobProgress = other.onBlobProgress;
    propertyEncryptor = other.propertyEncryptor;
    propertyDecryptor = other.propertyDecryptor;
    callbackContext = other.callbackContext;
    socketFactory = other.socketFactory;
}

void ReplParams::addCollections(const std::vector<C4ReplicationCollection>& collectionsToAdd) {
    for(const auto& c : collectionsToAdd) {
        _collectionVector.push_back(c);
    }
    collections = _collectionVector.data();
    collectionCount = _collectionVector.size();
}

AllocedDict ReplParams::setOptions(const AllocedDict& params, const AllocedDict& options) {
    AllocedDict result = params;
    for(Dict::iterator i(options); i; ++i) {
        result = repl::Options::updateProperties(result, i.keyString(), i.value());
    }
    return result;
}

ReplParams& ReplParams::setOptions(const AllocedDict& options) {
    _optionsDict = setOptions(_optionsDict, options);
    optionsDictFleece = _optionsDict.data();
    return *this;
}

ReplParams& ReplParams::setCollectionOptions(C4CollectionSpec collectionSpec, const AllocedDict &options) {
    for(auto& c : _collectionVector) {
        if(c.collection == collectionSpec) {
            _collectionsOptionsDict.emplace_back(
                    setOptions(AllocedDict(c.optionsDictFleece), options)
            );
            c.optionsDictFleece = _collectionsOptionsDict.back().data();
        }
    }
    return *this;
}

ReplParams& ReplParams::setDocIDs(const std::vector<std::unordered_map<alloc_slice, unsigned>>& docIDs) {
    for (size_t i = 0; i < docIDs.size(); ++i) {
        fleece::Encoder enc;
        enc.beginArray();

        for (const auto& d : docIDs[i]) {
            enc.writeString(d.first);
        }
        enc.endArray();
        Doc doc {enc.finish()};
        _collectionsOptionsDict.emplace_back(
                repl::Options::updateProperties(
                        AllocedDict(_collectionVector[i].optionsDictFleece),
                        kC4ReplicatorOptionDocIDs,
                        doc.root())
        );
        _collectionVector[i].optionsDictFleece = _collectionsOptionsDict.back().data();
    }
    return *this;
}

ReplParams& ReplParams::setCollectionOptions(const AllocedDict &options) {
    for(auto& c : _collectionVector) {
        _collectionsOptionsDict.emplace_back(
                setOptions(AllocedDict(c.optionsDictFleece), options)
        );
        c.optionsDictFleece = _collectionsOptionsDict.back().data();
    }
    return *this;
}

ReplParams& ReplParams::setPushPull(C4ReplicatorMode push, C4ReplicatorMode pull) {
    for(auto& c : _collectionVector) {
        c.push = push;
        c.pull = pull;
    }
    return *this;
}

C4ReplicatorParameters ReplParams::applyThisTo(C4ReplicatorParameters params) {
    _paramSetterOptions.emplace_back(setOptions(AllocedDict(params.optionsDictFleece), AllocedDict(optionsDictFleece)));
    params.optionsDictFleece = _paramSetterOptions.back().data();
    params.collections = _collectionVector.data();
    params.collectionCount = _collectionVector.size();
    if(onStatusChanged)
        params.onStatusChanged = onStatusChanged;
    if(onDocumentsEnded)
        params.onDocumentsEnded = onDocumentsEnded;
    if(onBlobProgress)
        params.onBlobProgress = onBlobProgress;
    if(propertyEncryptor)
        params.propertyEncryptor = propertyEncryptor;
    if(propertyDecryptor)
        params.propertyDecryptor = propertyDecryptor;
    if(callbackContext)
        params.callbackContext = callbackContext;
    if(socketFactory)
        params.socketFactory = socketFactory;

    return params;
}

std::function<void(C4ReplicatorParameters &)> ReplParams::paramSetter() {
    return [&](C4ReplicatorParameters& c4Params) {
        c4Params = applyThisTo(c4Params);
    };
}

ReplParams& ReplParams::setPushFilter(ValidationFunction pushFilter) {
    for(auto& c : _collectionVector) {
        c.pushFilter = pushFilter;
    }
    return *this;
}

ReplParams& ReplParams::setPullFilter(ValidationFunction pullFilter) {
    for(auto& c : _collectionVector) {
        c.pullFilter = pullFilter;
    }
    return *this;
}

ReplParams& ReplParams::setCollectionContext(int collectionIndex, void *callbackContext_) {
    _collectionVector[collectionIndex].callbackContext = callbackContext_;
    return *this;
}

ReplParams &ReplParams::setStatusCallback(StatusCallback statusCallback) {
    onStatusChanged = statusCallback;
    return *this;
}

ReplParams &ReplParams::setDocsEndedCallback(DocsEndedCallback docsEndedCallback) {
    onDocumentsEnded = docsEndedCallback;
    return *this;
}

ReplParams &ReplParams::setBlobProgressCallback(BlobProgressCallback blobProgressCallback) {
    onBlobProgress = blobProgressCallback;
    return *this;
}

ReplParams &ReplParams::setPropertyEncryptor(EncryptionCallback encryptionCallback) {
    propertyEncryptor = encryptionCallback;
    return *this;
}

ReplParams &ReplParams::setPropertyDecryptor(DecryptionCallback decryptionCallback) {
    propertyDecryptor = decryptionCallback;
    return *this;
}

ReplParams &ReplParams::setCallbackContext(void *callbackContext_) {
    for(auto& c : _collectionVector) {
        c.callbackContext = callbackContext_;
    }
    return *this;
}

ReplParams &ReplParams::setSocketFactory(C4SocketFactory *socketFactory_) {
    socketFactory = socketFactory_;
    return *this;
}
