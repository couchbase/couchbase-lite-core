//
// c4Replicator.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4ReplicatorTypes.h"
#include <functional>
#include <memory>
#include <span>
#include <vector>

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************

namespace fleece {
    class Dict;
    class MutableDict;
}  // namespace fleece

struct C4Replicator
    : public fleece::RefCounted
    , C4Base {
    // NOTE: Instances are created with database->newReplicator(...).

    static bool isValidDatabaseName(slice dbName) noexcept;

    static void validateRemote(const C4Address& addr, slice dbName);

    virtual void start(bool reset = false) noexcept = 0;
    virtual void stop() noexcept                    = 0;
    bool         retry() const;

    virtual void stopCallbacks() noexcept = 0;

    virtual void setHostReachable(bool) noexcept {}

    virtual void setSuspended(bool) noexcept = 0;

    void         setOptions(slice optionsDictFleece);
    virtual void setProgressLevel(C4ReplicatorProgressLevel) noexcept = 0;

    virtual C4ReplicatorStatus getStatus() const noexcept          = 0;
    virtual alloc_slice        getResponseHeaders() const noexcept = 0;

    alloc_slice pendingDocIDs(C4CollectionSpec) const;
    bool        isDocumentPending(slice docID, C4CollectionSpec) const;

#ifdef COUCHBASE_ENTERPRISE
    using PeerTLSCertificateValidator = std::function<bool(slice certData, std::string_view hostname)>;

    /// Registers a callback that can accept or reject a peer's certificate during the TLS handshake.
    virtual void setPeerTLSCertificateValidator(std::shared_ptr<PeerTLSCertificateValidator>) = 0;

    virtual C4Cert* C4NULLABLE getPeerTLSCertificate() const = 0;
#endif

    /** Extended, memory-safe version of `C4ReplicatorParameters`.
     *  The constructor copies all the pointed-to data into internal storage:
     *  - `optionsDictFleece`
     *  - `collections`
     *  - each collection's `name`, `scope` and `optionsDictFleece` */
    struct Parameters : C4ReplicatorParameters {
        Parameters();
        explicit Parameters(C4ReplicatorParameters const&);

        Parameters(Parameters const& params) : Parameters((C4ReplicatorParameters const&)params) {}

        std::span<C4ReplicationCollection> collections() noexcept { return _collections; }

        std::span<const C4ReplicationCollection> collections() const noexcept { return _collections; }

        /// The highest push and pull modes of any collections.
        std::pair<C4ReplicatorMode, C4ReplicatorMode> maxModes() const;

        C4ReplicationCollection& addCollection(C4ReplicationCollection const&);

        C4ReplicationCollection& addCollection(C4CollectionSpec const&, C4ReplicatorMode pushMode,
                                               C4ReplicatorMode pullMode);

        bool removeCollection(C4CollectionSpec const&);

        fleece::MutableDict copyOptions() const;       ///< Returns copy of options (never null)
        void                setOptions(fleece::Dict);  ///< Updates options Dict
        void                updateOptions(std::function<void(fleece::MutableDict)> const& callback);

      private:
        void makeAllocated(C4ReplicationCollection&);

        alloc_slice                          _options;
        std::vector<C4ReplicationCollection> _collections;
        std::vector<alloc_slice>             _slices;
    };
};

C4_ASSUME_NONNULL_END
