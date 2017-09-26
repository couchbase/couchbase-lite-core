//
//  DBEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"
#include "c4Replicator.h"

class JSONEndpoint;


class DbEndpoint : public Endpoint {
public:
    DbEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty, const Endpoint*) override;
    virtual void copyTo(Endpoint *dst, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    void pushTo(DbEndpoint*);
    void exportTo(JSONEndpoint*);
    void importFrom(JSONEndpoint*);

    void onStateChanged(C4ReplicatorStatus status);
    void onDocError(bool pushing,
                    C4String docID,
                    C4Error error,
                    bool transient);

private:
    void enterTransaction();
    void commit();
    void startLine();

    c4::ref<C4Database> _db;
    unsigned _transactionSize {0};
    bool _inTransaction {false};

    // Replication mode only:
    DbEndpoint* _dstDb;
    c4::ref<C4Replicator> _repl;
    bool _needNewline {false};

    static constexpr unsigned kMaxTransactionSize = 1000;
};


