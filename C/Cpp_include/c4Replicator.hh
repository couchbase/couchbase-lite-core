//
// c4Replicator.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#pragma once
#include "c4Base.hh"
#include "c4ReplicatorTypes.h"

C4_ASSUME_NONNULL_BEGIN


struct C4Replicator : public fleece::RefCounted, public C4Base {
    // NOTE: Instances are created with database->newReplicator(...).

    static bool isValidDatabaseName(slice dbName) noexcept;

    static bool isValidRemote(const C4Address &addr,
                              slice dbName,
                              C4Error* C4NULLABLE outError) noexcept;

    static void validateRemote(const C4Address &addr,
                               slice dbName);

    static bool addressFromURL(slice URL,
                               C4Address &outAddress,
                               slice* C4NULLABLE outDBName);

    static alloc_slice addressToURL(const C4Address&);

    virtual void start(bool reset =false) =0;
    virtual void stop() =0;
    void retry();

    virtual void stopCallbacks() =0;

    virtual void setHostReachable(bool) { }
    virtual void setSuspended(bool) =0;
    void setOptions(slice optionsDictFleece);
    virtual void setProgressLevel(C4ReplicatorProgressLevel) =0;

    virtual C4ReplicatorStatus status() const =0;

    virtual alloc_slice responseHeaders() const =0;

    alloc_slice pendingDocIDs() const;
    bool isDocumentPending(slice docID) const;

#ifdef COUCHBASE_ENTERPRISE
    C4Cert* C4NULLABLE peerTLSCertificate() const;
#endif
};

C4_ASSUME_NONNULL_END
