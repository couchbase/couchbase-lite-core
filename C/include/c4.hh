//
// c4.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#ifndef __cplusplus
#error "This is C++ only"
#endif

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.h"
#include "c4Listener.h"
#include <assert.h>


namespace c4 {

    // The functions the ref<> template calls to free a reference.
    static inline void freeRef(C4Database* c)          {c4db_free(c);}
    static inline void freeRef(C4RawDocument* c)       {c4raw_free(c);}
    static inline void freeRef(C4Document* c)          {c4doc_free(c);}
    static inline void freeRef(C4DocEnumerator* c)     {c4enum_free(c);}
    static inline void freeRef(C4ExpiryEnumerator* c)  {c4exp_free(c);}
    static inline void freeRef(C4DatabaseObserver* c)  {c4dbobs_free(c);}
    static inline void freeRef(C4DocumentObserver* c)  {c4docobs_free(c);}
    static inline void freeRef(C4QueryEnumerator* c)   {c4queryenum_free(c);}
    static inline void freeRef(C4Query* c)             {c4query_free(c);}
    static inline void freeRef(C4ReadStream* c)        {c4stream_close(c);}
    static inline void freeRef(C4WriteStream* c)       {c4stream_closeWriter(c);}
    static inline void freeRef(C4Replicator* c)        {c4repl_free(c);}
    static inline void freeRef(C4Listener* c)          {c4listener_free(c);}


    /** A simple little smart pointer that frees the C4 object when it leaves scope. */
    template <class T>
    class ref {
    public:
        ref()                       :_obj(nullptr) { }
        ref(T *t)                   :_obj(t) { }
        ref(ref &&r)                :_obj(r._obj) {r._obj = nullptr;}
        ~ref()                      {if (_obj) freeRef(_obj);}

        operator T* () const        {return _obj;}
        T* operator -> () const     {return _obj;}

        ref& operator=(T *t)        {if (_obj) freeRef(_obj); _obj = t; return *this;}
        ref& operator=(ref &&r)     {_obj = r._obj; r._obj = nullptr; return *this;}

    private:
        ref(const ref&) =delete;
        ref& operator=(const ref&) =delete;   // would require ref-counting

        T* _obj;
    };


    /** Manages a C4SliceResult, letting you treat it as a slice and freeing it for you. */
    struct sliceResult : public fleece::slice {
    public:
        explicit sliceResult(C4SliceResult sr)   :slice(sr.buf, sr.size) { }
        ~sliceResult()                  {c4slice_free(*(C4SliceResult*)this);}

        sliceResult(const sliceResult&) =delete;
        sliceResult& operator= (const sliceResult&) =delete;
    };

    using stringResult = sliceResult;


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

        bool begin(C4Error *error) {
            assert(!_active);
            if (!c4db_beginTransaction(_db, error))
                return false;
            _active = true;
            return true;
        }

        bool end(bool commit, C4Error *error) {
            assert(_active);
            _active = false;
            return c4db_endTransaction(_db, commit, error);
        }

        bool commit(C4Error *error)     {return end(true, error);}
        bool abort(C4Error *error)      {return end(false, error);}

        bool active() const             {return _active;}

    private:
        C4Database *_db;
        bool _active {false};
    };
}
