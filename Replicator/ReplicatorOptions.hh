//
//  ReplicatorOptions.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorTypes.h"
#include "c4ReplicatorHelpers.hh"
#include "c4Database.hh"
#include "ReplicatorTypes.hh"
#include "fleece/RefCounted.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include "NumConversion.hh"
#include <unordered_map>

namespace litecore::repl {

    using namespace fleece;

    /** Replication configuration options */
    class Options final : public fleece::RefCounted {
      public:
        //---- Public fields:

        using Mode              = C4ReplicatorMode;
        using Validator         = C4ReplicatorValidationFunction;
        using PropertyEncryptor = C4ReplicatorPropertyEncryptionCallback;
        using PropertyDecryptor = C4ReplicatorPropertyDecryptionCallback;

        fleece::AllocedDict                    properties;
        PropertyEncryptor                      propertyEncryptor{nullptr};
        PropertyDecryptor                      propertyDecryptor{nullptr};
        void*                                  callbackContext{nullptr};
        std::atomic<C4ReplicatorProgressLevel> progressLevel{kC4ReplProgressOverall};

        bool collectionAware() const { return _mutables._collectionAware; }

        bool isActive() const { return _mutables._isActive; }

        const std::unordered_map<C4CollectionSpec, size_t>& collectionSpecToIndex() const {
            return _mutables._collectionSpecToIndex;
        }

        //---- Constructors/factories:

        Options(Mode push_, Mode pull_) {
            setCollectionOptions(push_, pull_);
            constructorCheck();
        }

        template <class SLICE>
        Options(Mode push_, Mode pull_, SLICE propertiesFleece) : properties(propertiesFleece) {
            setCollectionOptions(push_, pull_);
            constructorCheck();
        }

        explicit Options(C4ReplicatorParameters params)
            : properties(params.optionsDictFleece)
            , propertyEncryptor(params.propertyEncryptor)
            , propertyDecryptor(params.propertyDecryptor)
            , callbackContext(params.callbackContext) {
            setCollectionOptions(params);
            constructorCheck();
        }

        Options(const Options& opt)  // copy ctor, required because std::atomic doesn't have one
            : propertyEncryptor(opt.propertyEncryptor)
            , propertyDecryptor(opt.propertyDecryptor)
            , callbackContext(opt.callbackContext)
            , properties(slice(opt.properties.data()))  // copy data, bc dtor wipes it
            , progressLevel(opt.progressLevel.load()) {
            setCollectionOptions(opt);
            constructorCheck();
        }

        static Options pushing(Mode mode = kC4OneShot, C4CollectionSpec coll = kC4DefaultCollectionSpec) {
            return Options(C4ReplParamsOneCollection(coll, mode, kC4Disabled));
        }

        static Options pulling(Mode mode = kC4OneShot, C4CollectionSpec coll = kC4DefaultCollectionSpec) {
            return Options(C4ReplParamsOneCollection(coll, kC4Disabled, mode));
        }

        static Options pushpull(Mode mode = kC4OneShot, C4CollectionSpec coll = kC4DefaultCollectionSpec) {
            return Options(C4ReplParamsOneCollection(coll, mode, mode));
        }

        static Options passive(C4CollectionSpec coll = kC4DefaultCollectionSpec) {
            return Options(C4ReplParamsOneCollection(coll, kC4Passive, kC4Passive));
        }

        //---- Property accessors:

        bool setProgressLevel(C4ReplicatorProgressLevel level) { return progressLevel.exchange(level) != level; }

        fleece::slice filter() const { return properties[kC4ReplicatorOptionFilter].asString(); }

        fleece::Dict filterParams() const { return properties[kC4ReplicatorOptionFilterParams].asDict(); }

        bool skipDeleted() const { return boolProperty(kC4ReplicatorOptionSkipDeleted); }

        bool noIncomingConflicts() const { return boolProperty(kC4ReplicatorOptionNoIncomingConflicts); }

        bool noOutgoingConflicts() const { return boolProperty(kC4ReplicatorOptionNoIncomingConflicts); }

        bool disableDeltaSupport() const { return boolProperty(kC4ReplicatorOptionDisableDeltas); }

        bool disablePropertyDecryption() const { return boolProperty(kC4ReplicatorOptionDisablePropertyDecryption); }

        bool enableAutoPurge() const {
            if ( !properties[kC4ReplicatorOptionAutoPurge] ) return true;
            return boolProperty(kC4ReplicatorOptionAutoPurge);
        }

        bool acceptParentDomainCookies() const {
            if ( !properties[kC4ReplicatorOptionAcceptParentDomainCookies] ) return false;
            return boolProperty(kC4ReplicatorOptionAcceptParentDomainCookies);
        }

        /** Returns a string that uniquely identifies the remote database; by default its URL,
            or the 'remoteUniqueID' option if that's present (for P2P dbs without stable URLs.) */
        fleece::slice remoteDBIDString(fleece::slice remoteURL) const {
            auto uniqueID = properties[kC4ReplicatorOptionRemoteDBUniqueID].asString();
            return uniqueID ? uniqueID : remoteURL;
        }

        fleece::Array arrayProperty(const char* name) const { return properties[name].asArray(); }

        fleece::Dict dictProperty(const char* name) const { return properties[name].asDict(); }

        //---- Property setters (used only by tests)

        template <class T>
        static fleece::AllocedDict updateProperties(const fleece::AllocedDict& properties, fleece::slice name,
                                                    T value) {
            fleece::Encoder enc;
            enc.beginDict();
            if ( std::is_same<decltype(value), bool>::value ) {
                enc.writeKey(name);
                enc.writeBool((bool)value);
            } else if ( std::is_arithmetic<decltype(value)>::value || value ) {
                enc.writeKey(name);
                enc << value;
            }
            for ( fleece::Dict::iterator i(properties); i; ++i ) {
                fleece::slice key = i.keyString();
                if ( key != name ) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.endDict();
            return fleece::AllocedDict(enc.finish());
        }

        /** Sets/clears the value of a property.
            Warning: This rewrites the backing store of the properties, invalidating any
            Fleece value pointers or slices previously accessed from it. */
        template <class T>
        Options& setProperty(fleece::slice name, T value) {
            properties = Options::updateProperties(properties, name, value);
            return *this;
        }

        Options& setNoIncomingConflicts() { return setProperty(kC4ReplicatorOptionNoIncomingConflicts, true); }

        Options& setNoDeltas() { return setProperty(kC4ReplicatorOptionDisableDeltas, true); }

        Options& setNoPropertyDecryption() { return setProperty(kC4ReplicatorOptionDisablePropertyDecryption, true); }

        bool boolProperty(slice property) const { return properties[property].asBool(); }

        explicit operator std::string() const;

        // Collection Options:

        // The BLIP message, getCollections, specifies that the body consist of an array of
        // collection paths, e.g. '[“scope/foo”,”bar”,”zzz/buzz”]'. So, we convert the
        // CollecttionSpec given in C4ReplicatorParamters to slash separated path.
        static alloc_slice collectionSpecToPath(C4CollectionSpec spec, bool omitDefaultScope = true) {
            if ( spec.scope == nullslice || spec.name == nullslice ) { return nullslice; }
            bool addScope = true;
            if ( FLSlice_Compare(spec.scope, kC4DefaultScopeID) == 0 && omitDefaultScope ) { addScope = false; }
            size_t size = addScope ? spec.scope.size + 1 : 0;
            size += spec.name.size;
            alloc_slice ret(size);
            void*       buf        = const_cast<void*>(ret.buf);
            size_t      nameOffset = 0;
            if ( addScope ) {
                slice(spec.scope).copyTo(buf);
                ((uint8_t*)buf)[spec.scope.size] = '.';
                nameOffset                       = spec.scope.size + 1;
            }
            slice(spec.name).copyTo((uint8_t*)buf + nameOffset);
            return ret;
        }

        static C4CollectionSpec collectionPathToSpec(slice path) {
            const uint8_t* slash = path.findByte((uint8_t)'.');
            slice          scope = kC4DefaultScopeID;
            slice          name;
            if ( slash != nullptr ) {
                scope = slice{path.buf, static_cast<size_t>(slash - static_cast<const uint8_t*>(path.buf))};
                name  = slice{slash + 1, path.size - scope.size - 1};
            } else {
                name = path;
            }
            return {name, scope};
        }

        inline static alloc_slice const kDefaultCollectionPath = collectionSpecToPath(kC4DefaultCollectionSpec, false);

        struct CollectionOptions {
            C4CollectionSpec collectionSpec{};

            C4ReplicatorMode push{};
            C4ReplicatorMode pull{};

            fleece::AllocedDict properties;

            C4ReplicatorValidationFunction pushFilter{nullptr};
            C4ReplicatorValidationFunction pullFilter{nullptr};
            void*                          callbackContext{nullptr};

          private:
            alloc_slice collectionPath;

          public:
            explicit CollectionOptions(C4CollectionSpec collectionSpec_) {
                collectionPath = collectionSpecToPath(collectionSpec_);
                collectionSpec = collectionPathToSpec(collectionPath);
            }

            CollectionOptions(C4CollectionSpec collectionSpec_, C4Slice properties_) : properties(properties_) {
                collectionPath = collectionSpecToPath(collectionSpec_);
                collectionSpec = collectionPathToSpec(collectionPath);
            }

            template <class T>
            CollectionOptions& setProperty(fleece::slice name, T value) {
                properties = Options::updateProperties(properties, name, value);
                return *this;
            }
        };

        std::vector<CollectionOptions> collectionOpts;

        // Post-conditions:
        //   collectionOpts.size() > 0
        //   collectionAware == false if and only if collectionOpts.size() == 1 &&
        //                                           collectionOpts[0].collectionPath == defaultCollectionPath
        //   isActive == true ? all collections are active
        //                    : all collections are passive.
        inline void verify() const;

        size_t collectionCount() const { return _mutables._workingCollections.size(); }

        Mode push(CollectionIndex i) const { return _mutables._workingCollections[i].push; }

        Mode pull(CollectionIndex i) const { return _mutables._workingCollections[i].pull; }

        Validator pushFilter(CollectionIndex i) const { return _mutables._workingCollections[i].pushFilter; }

        Validator pullFilter(CollectionIndex i) const { return _mutables._workingCollections[i].pullFilter; }

        void* collectionCallbackContext(CollectionIndex i) const {
            return _mutables._workingCollections[i].callbackContext;
        }

        fleece::Array channels(CollectionIndex i) const {
            return _mutables._workingCollections[i].properties[kC4ReplicatorOptionChannels].asArray();
        }

        fleece::Array docIDs(CollectionIndex i) const {
            return _mutables._workingCollections[i].properties[kC4ReplicatorOptionDocIDs].asArray();
        }

        fleece::alloc_slice collectionPath(CollectionIndex i) const {
            return collectionSpecToPath(_mutables._workingCollections[i].collectionSpec);
        }

        C4CollectionSpec collectionSpec(CollectionIndex i) const {
            return _mutables._workingCollections[i].collectionSpec;
        }

        CollectionIndex workingCollectionCount() const {
            return fleece::narrow_cast<CollectionIndex>(_mutables._workingCollections.size());
        }

        // RearrangeCollections() is called only by the passive replicator. For the passive replicator, we presume
        // that the order of the collection properties is not important. So, we take it as legit to permutate
        // it in const method. (The Replicator holds a const Options object.) It is supposed to be called as it
        // starts to interact with the active replicator.
        // "collections" is a list of CollectionSpecs that the active replicatore proposes to replicate, and the
        // order will be used as index to refer to respective collections.
        // Post-conditions: collectionOpts[i] and collections[i] share the same CollectionSpec if
        //                    collections[i] is found in collectionOpts;
        //                  Otherwise, an empty collectionOptions is inserted in collectionOpts[i].
        // By empty collectionOptions we mean the collection path is a null slice.

        void rearrangeCollections(const std::vector<C4CollectionSpec>& activeCollections) const {
            DebugAssert(!_mutables._isActive);

            // Clear out the current spec to index map so there is not
            // any stale info in it, but keep a copy to search for
            // existing entries
            auto collectionSpecToIndexOld = _mutables._collectionSpecToIndex;
            _mutables._collectionSpecToIndex.clear();
            _mutables._workingCollections.clear();
            _mutables._workingCollections.reserve(activeCollections.size());

            for ( size_t activeIndex = 0; activeIndex < activeCollections.size(); ++activeIndex ) {
                auto foundEntry = collectionSpecToIndexOld.find(activeCollections[activeIndex]);
                if ( foundEntry == collectionSpecToIndexOld.end() ) {
                    _mutables._workingCollections.emplace_back(C4CollectionSpec{nullslice, nullslice});
                } else {
                    _mutables._workingCollections.push_back(collectionOpts[foundEntry->second]);
                    _mutables._collectionSpecToIndex[activeCollections[activeIndex]] = activeIndex;
                }
            }
        }

        void rearrangeCollectionsFor3_0_Client() const {
            _mutables._collectionAware = false;
            std::vector<C4CollectionSpec> activeCollections{kC4DefaultCollectionSpec};
            rearrangeCollections(activeCollections);
        }

        static const std::unordered_set<slice> kWhiteListOfKeysToLog;

      private:
        inline void setCollectionOptions(Mode push, Mode pull);
        inline void setCollectionOptions(C4ReplicatorParameters params);
        inline void setCollectionOptions(const Options& opt);
        inline void constructorCheck();

        struct Mutables {
            mutable std::vector<CollectionOptions>               _workingCollections;
            mutable bool                                         _collectionAware{true};
            mutable bool                                         _isActive{true};
            mutable std::unordered_map<C4CollectionSpec, size_t> _collectionSpecToIndex;
        };

        Mutables _mutables;
    };

    inline void Options::setCollectionOptions(Mode push, Mode pull) {
        collectionOpts.reserve(1);
        auto& back = collectionOpts.emplace_back(kC4DefaultCollectionSpec);
        back.push  = push;
        back.pull  = pull;
    }

    inline void Options::setCollectionOptions(C4ReplicatorParameters params) {
        collectionOpts.reserve(params.collectionCount);
        for ( unsigned i = 0; i < params.collectionCount; ++i ) {
            C4ReplicationCollection& c4Coll = params.collections[i];
            auto&                    back   = collectionOpts.emplace_back(c4Coll.collection, c4Coll.optionsDictFleece);
            back.push                       = c4Coll.push;
            back.pull                       = c4Coll.pull;
            back.pushFilter                 = c4Coll.pushFilter;
            back.pullFilter                 = c4Coll.pullFilter;
            back.callbackContext            = c4Coll.callbackContext;
        }
    }

    inline void Options::setCollectionOptions(const Options& opt) {
        collectionOpts.reserve(opt.collectionOpts.size());
        for ( auto& collOpts : opt.collectionOpts ) {
            auto& back           = collectionOpts.emplace_back(collOpts.collectionSpec, collOpts.properties.data());
            back.push            = collOpts.push;
            back.pull            = collOpts.pull;
            back.pushFilter      = collOpts.pushFilter;
            back.pullFilter      = collOpts.pullFilter;
            back.callbackContext = collOpts.callbackContext;
        }
    }

    inline void Options::verify() const {
        if ( collectionOpts.empty() ) {
            throw error(error::LiteCore, error::InvalidParameter,
                        "Invalid replicator configuration: requiring at least one collection");
        }

        for ( size_t i = collectionOpts.size(); i-- > 0; ) {
            if ( collectionOpts[i].collectionSpec.name.size == 0 ) {
                throw error(error::LiteCore, error::InvalidParameter,
                            "Invalid replicator configuration: a collection without name");
            }
            if ( collectionOpts[i].push == kC4Disabled && collectionOpts[i].pull == kC4Disabled ) {
                throw error(error::LiteCore, error::InvalidParameter,
                            "Invalid replicator configuration: a collection with both push and pull disabled");
            }
        }

        // Assertion: collectionOpts contains no disabled collections
        // (of which both push and pull are disabled)

        // Do not allow active and passive to be mixed in the same replicator.

        unsigned passCount = 0;
        unsigned actiCount = 0;
        for ( auto& c : collectionOpts ) {
            if ( c.push == kC4Passive ) ++passCount;
            else if ( c.push > kC4Passive )
                ++actiCount;
            if ( c.pull == kC4Passive ) ++passCount;
            else if ( c.pull > kC4Passive )
                ++actiCount;

            if ( passCount * actiCount > 0 ) {
                throw error(error::LiteCore, error::InvalidParameter,
                            "Invalid replicator configuration: the collection list includes"
                            " both passive and active ReplicatorMode");
            }
        }
        _mutables._isActive = actiCount > 0;

        // Do not mix one-shot and continous modes in one replicator.

        unsigned oneshot    = 0;
        unsigned continuous = 0;
        if ( _mutables._isActive && collectionOpts.size() > 1 ) {
            for ( const auto& c : collectionOpts ) {
                if ( c.push == kC4OneShot ) ++oneshot;
                else if ( c.push == kC4Continuous )
                    ++continuous;
                if ( c.pull == kC4OneShot ) ++oneshot;
                else if ( c.pull == kC4Continuous )
                    ++continuous;

                if ( oneshot * continuous > 0 ) {
                    throw error(error::LiteCore, error::InvalidParameter,
                                "Invalid replicator configuration: kC4OneShot and kC4Continuous modes cannot be mixed "
                                "in one replicator.");
                }
            }
        }

        if ( collectionOpts.size() == 1 ) {
            auto spec = collectionOpts[0].collectionSpec;
            if ( spec == kC4DefaultCollectionSpec ) { _mutables._collectionAware = false; }
        }
    }

    // Post-conditions:
    //   - collectionOpts contains no duplicated collection.
    inline void Options::constructorCheck() {
        Assert(collectionOpts.size() < kNotCollectionIndex);
        // _workingCollections will be cleared and reordered later for passive
        // replicators, but stay the same for active

        _mutables._workingCollections = collectionOpts;

        // Create the mapping from CollectionSpec to the index to collctionOpts
        for ( size_t i = 0; i < collectionOpts.size(); ++i ) {
            auto spec = collectionOpts[i].collectionSpec;
            bool b;
            std::tie(std::ignore, b) = _mutables._collectionSpecToIndex.insert(std::make_pair(spec, i));
            if ( !b ) {
                throw error(error::LiteCore, error::InvalidParameter,
                            "Invalid replicator configuration: the collection list contains duplicated collections.");
            }
        }
    }

}  // namespace litecore::repl
