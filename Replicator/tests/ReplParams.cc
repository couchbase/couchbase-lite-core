//
// Created by Callum Birks on 07/11/2022.
//

#include "ReplParams.hh"

ReplParams::ReplParams(std::vector<C4ReplicationCollection> collections_, const AllocedDict& options,
                       StatusCallback statusCallback, DocsEndedCallback docsEndedCallback,
                       BlobProgressCallback blobProgressCallback, EncryptionCallback encryptionCallback,
                       DecryptionCallback decryptionCallback, void *callbackContext_, C4SocketFactory *socketFactory_)
{
    _collectionVector = std::vector<C4ReplicationCollection>(collections_);
    collections = _collectionVector.data();
    collectionCount = _collectionVector.size();
    _optionsDict = options;
    optionsDictFleece = _optionsDict.data();
    onStatusChanged = statusCallback;
    onDocumentsEnded = docsEndedCallback;
    propertyEncryptor = encryptionCallback;
    propertyDecryptor = decryptionCallback;
    callbackContext = callbackContext_;
    socketFactory = socketFactory_;
}

ReplParams::ReplParams(const ReplParams &other) {
    _collectionVector = { other._collectionVector };
    _collectionsOptionsDict = { other._collectionsOptionsDict };
    _paramSetterOptions = { other._paramSetterOptions };
    _optionsDict = AllocedDict(other._optionsDict);
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

void ReplParams::setOptions(AllocedDict options) {
    _optionsDict = setOptions(AllocedDict(optionsDictFleece), options);
    optionsDictFleece = _optionsDict.data();
}

void ReplParams::setCollectionOptions(C4CollectionSpec collectionSpec, const AllocedDict &options) {
    for(auto& c : _collectionVector) {
        if(c.collection == collectionSpec) {
            _collectionsOptionsDict.emplace_back(
                    setOptions(AllocedDict(c.optionsDictFleece), options)
            );
            c.optionsDictFleece = _collectionsOptionsDict.back().data();
        }
    }
}

void ReplParams::setCollectionOptions(const AllocedDict &options) {
    for(auto& c : _collectionVector) {
        _collectionsOptionsDict.emplace_back(
                setOptions(AllocedDict(c.optionsDictFleece), options)
        );
        c.optionsDictFleece = _collectionsOptionsDict.back().data();
    }
}

void ReplParams::setPushPull(C4ReplicatorMode push, C4ReplicatorMode pull) {
    for(auto& c : _collectionVector) {
        c.push = push;
        c.pull = pull;
    }
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

void ReplParams::setPushFilter(ValidationFunction pushFilter) {
    for(auto& c : _collectionVector) {
        c.pushFilter = pushFilter;
    }
}

void ReplParams::setPullFilter(ValidationFunction pullFilter) {
    for(auto& c : _collectionVector) {
        c.pullFilter = pullFilter;
    }
}
