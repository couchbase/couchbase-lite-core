//
//  DBEndpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Endpoint.hh"

class JSONEndpoint;


class DbEndpoint : public Endpoint {
public:
    DbEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint *dst, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    void pushTo(DbEndpoint*);
    void exportTo(JSONEndpoint*);
    void importFrom(JSONEndpoint*);

private:
    void commit();

    c4::ref<C4Database> _db;
    unsigned _transactionSize {0};

    static constexpr unsigned kMaxTransactionSize = 1000;
};


