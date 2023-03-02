//
// Created by Callum Birks on 24/01/2023.
//

#include "ReplParams.hh"
#include "ReplicatorOptions.hh"

ReplParams::ReplParams(C4ReplicatorMode push_, C4ReplicatorMode pull_)
    : C4ReplicatorParameters()
{
    push = push_;
    pull = pull_;
    _optionsDict = {};
    optionsDictFleece = _optionsDict.data();
}

ReplParams &ReplParams::setOptions(const AllocedDict &options) {
    _optionsDict = setOptions(_optionsDict, options);
    optionsDictFleece = _optionsDict.data();
    return *this;
}

ReplParams &ReplParams::setDocIDs(const std::unordered_map<alloc_slice, unsigned int> &docIDs) {
    fleece::Encoder enc;
    enc.beginArray();
    for (const auto& d : docIDs) {
        enc.writeString(d.first);
    }
    enc.endArray();
    Doc opts {enc.finish()};
    _optionsDict = updateProperties(
                _optionsDict, kC4ReplicatorOptionDocIDs, opts.root()
            );
    optionsDictFleece = _optionsDict.data();
    return *this;
}

ReplParams &ReplParams::setPushPull(C4ReplicatorMode push_, C4ReplicatorMode pull_) {
    push = push_;
    pull = pull_;
    return *this;
}

ReplParams &ReplParams::setPushFilter(ValidationFunction pushFilter_) {
    pushFilter = pushFilter_;
    return *this;
}

ReplParams &ReplParams::setPullFilter(ValidationFunction pullFilter_) {
    validationFunc = pullFilter_;
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

C4ReplicatorParameters ReplParams::applyThisTo(C4ReplicatorParameters params) {
    _paramSetterOptions.emplace_back(setOptions(AllocedDict(params.optionsDictFleece), _optionsDict));
    params.optionsDictFleece = _paramSetterOptions.back().data();
    params.push = push;
    params.pull = pull;
    if (pushFilter)
        params.pushFilter = pushFilter;
    if (validationFunc)
        params.validationFunc = validationFunc;
    if (onStatusChanged)
        params.onStatusChanged = onStatusChanged;
    if (onDocumentsEnded)
        params.onDocumentsEnded = onDocumentsEnded;
    if (onBlobProgress)
        params.onBlobProgress = onBlobProgress;
    if (propertyEncryptor)
        params.propertyEncryptor = propertyEncryptor;
    if (propertyDecryptor)
        params.propertyDecryptor = propertyDecryptor;
    if (callbackContext)
        params.callbackContext = callbackContext;
    if (socketFactory)
        params.socketFactory = socketFactory;

    return params;
}

std::function<void(C4ReplicatorParameters &)> ReplParams::paramSetter() {
    return [&](C4ReplicatorParameters& c4Params) {
        c4Params = applyThisTo(c4Params);
    };
}

AllocedDict ReplParams::setOptions(const AllocedDict &params, const AllocedDict &options) {
    AllocedDict result = params;
    for(Dict::iterator i(options); i; ++i) {
        result = updateProperties(result, i.keyString(), i.value());
    }
    return result;
}
