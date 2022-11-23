//
// Created by Callum Birks on 07/11/2022.
//

#include "ReplParams.hh"

ReplParams::ReplParams(const std::vector<C4ReplicationCollection>& collections_)
{
    _collectionVector = { collections_ };
    collections = _collectionVector.data();
    collectionCount = _collectionVector.size();
    _optionsDict = {};
    optionsDictFleece = _optionsDict.data();
    onStatusChanged = nullptr;
    onDocumentsEnded = nullptr;
    onBlobProgress = nullptr;
    propertyDecryptor = nullptr;
    propertyEncryptor = nullptr;
    callbackContext = nullptr;
    socketFactory = nullptr;
}

ReplParams::ReplParams(const ReplParams &other) {
    _collectionVector = { other._collectionVector };
    _collectionsOptionsDict = { other._collectionsOptionsDict };
    _paramSetterOptions = { other._paramSetterOptions };
    _optionsDict = { other._optionsDict };
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

void ReplParams::addCollections(std::vector<C4ReplicationCollection> collectionsToAdd) {
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

ReplParams& ReplParams::setOptions(AllocedDict options) {
    _optionsDict = setOptions(AllocedDict(optionsDictFleece), options);
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
        if(c.callbackContext)
            c.pushFilter = pushFilter;
    }
    return *this;
}

ReplParams& ReplParams::setPullFilter(ValidationFunction pullFilter) {
    for(auto& c : _collectionVector) {
        if(c.callbackContext)
            c.pullFilter = pullFilter;
    }
    return *this;
}

ReplParams& ReplParams::setCollectionContext(void *callbackContext_) {
    for(auto& c : _collectionVector) {
        c.callbackContext = callbackContext_;
    }
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
    callbackContext = callbackContext_;
    return *this;
}

ReplParams &ReplParams::setSocketFactory(C4SocketFactory *socketFactory_) {
    socketFactory = socketFactory_;
    return *this;
}
