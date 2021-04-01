//
//  c4Transaction.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/2/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once

#ifndef __cplusplus
#error "This is C++ only"
#endif

#include "c4Database.h"
#include <assert.h>

C4_ASSUME_NONNULL_BEGIN

namespace c4 {

    /** Manages a transaction safely. The begin() method calls c4db_beginTransaction, then commit()
        or abort() end it. If the Transaction object exits scope when it's been begun but not yet
        ended, it aborts the transaction. */
    class Transaction {
    public:
        Transaction(C4Database *db)
        :_db(db)
        { }

        ~Transaction() {
            if (_active)
                abort(nullptr);
        }

        bool begin(C4Error* C4NULLABLE error) {
            assert(!_active);
            if (!c4db_beginTransaction(_db, error))
                return false;
            _active = true;
            return true;
        }

        bool end(bool commit, C4Error* C4NULLABLE error) {
            assert(_active);
            _active = false;
            return c4db_endTransaction(_db, commit, error);
        }

        bool commit(C4Error* C4NULLABLE error)     {return end(true, error);}
        bool abort(C4Error* C4NULLABLE error)      {return end(false, error);}

        bool active() const             {return _active;}

    private:
        C4Database *_db;
        bool _active {false};
    };

}

C4_ASSUME_NONNULL_END
