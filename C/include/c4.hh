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

#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "c4.h"
#include "c4Document+Fleece.h"
#include "c4Listener.h"
#include <assert.h>


// C4Error equality tests:
// (These have to be in the global namespace, because C4Error is...)
static inline bool operator== (C4Error a, C4Error b) {
    return a.code == b.code && a.domain == b.domain;
}
static inline bool operator!= (C4Error a, C4Error b) {
    return !(a == b);
}


namespace c4 {

    /** A simple little smart pointer that frees the C4 object when it leaves scope. */
    template <class T>
    class ref {
    public:
        ref()                       :_obj(nullptr) { }
        ref(T *t)                   :_obj(t) { }
        ref(ref &&r)                :_obj(r._obj) {r._obj = nullptr;}
        ref(const ref &r)           :_obj(retainRef(r._obj)) { }
        ~ref()                      {freeRef(_obj);}

        operator T* () const        {return _obj;}
        T* operator -> () const     {return _obj;}

        ref& operator=(T *t)        {if (_obj) freeRef(_obj); _obj = t; return *this;}
        ref& operator=(ref &&r)     {_obj = r._obj; r._obj = nullptr; return *this;}
        ref& operator=(const ref &r){*this = retainRef(r._obj); return *this;}

    private:
        // The functions the ref<> template calls to free a reference.
        static inline void freeRef(C4Database* c)          {c4db_release(c);}
        static inline void freeRef(C4RawDocument* c)       {c4raw_free(c);}
        static inline void freeRef(C4Document* c)          {c4doc_release(c);}
        static inline void freeRef(C4DocEnumerator* c)     {c4enum_free(c);}
        static inline void freeRef(C4DatabaseObserver* c)  {c4dbobs_free(c);}
        static inline void freeRef(C4DocumentObserver* c)  {c4docobs_free(c);}
        static inline void freeRef(C4Query* c)             {c4query_release(c);}
        static inline void freeRef(C4QueryEnumerator* c)   {c4queryenum_release(c);}
        static inline void freeRef(C4QueryObserver* c)     {c4queryobs_free(c);}
        static inline void freeRef(C4ReadStream* c)        {c4stream_close(c);}
        static inline void freeRef(C4WriteStream* c)       {c4stream_closeWriter(c);}
        static inline void freeRef(C4Replicator* c)        {c4repl_free(c);}
        static inline void freeRef(C4Listener* c)          {c4listener_free(c);}
        static inline void freeRef(C4Cert* c)              {c4cert_release(c);}
        static inline void freeRef(C4KeyPair* c)           {c4keypair_release(c);}

        static inline C4Database* retainRef(C4Database* c) {return c4db_retain(c);}
        static inline C4Document* retainRef(C4Document* c) {return c4doc_retain(c);}
        static inline C4Query*    retainRef(C4Query* c)    {return c4query_retain(c);}
        static inline C4QueryEnumerator* retainRef(C4QueryEnumerator* c) {return c4queryenum_retain(c);}
        static inline C4Cert*     retainRef(C4Cert* c)     {return c4cert_retain(c);}
        static inline C4KeyPair*  retainRef(C4KeyPair* c)  {return c4keypair_retain(c);}

        T* _obj;
    };


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


    static inline fleece::Doc getFleeceDoc(C4Document *doc) {
        return fleece::Doc(c4doc_createFleeceDoc(doc), false);
    }


#define c4error_descriptionStr(ERR)     alloc_slice(c4error_getDescription(ERR)).asString().c_str()
    
}
