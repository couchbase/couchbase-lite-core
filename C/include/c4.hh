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
#include "c4Base.h"
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

    // The functions the ref<> template calls to free a reference.
    static inline void releaseRef(C4Cert* c)              noexcept {c4cert_release(c);}
    static inline void releaseRef(C4Database* c)          noexcept {c4db_release(c);}
    static inline void releaseRef(C4DatabaseObserver* c)  noexcept {c4dbobs_free(c);}
    static inline void releaseRef(C4DocEnumerator* c)     noexcept {c4enum_free(c);}
    static inline void releaseRef(C4Document* c)          noexcept {c4doc_release(c);}
    static inline void releaseRef(C4DocumentObserver* c)  noexcept {c4docobs_free(c);}
    static inline void releaseRef(C4KeyPair* c)           noexcept {c4keypair_release(c);}
    static inline void releaseRef(C4Listener* c)          noexcept {c4listener_free(c);}
    static inline void releaseRef(C4Query* c)             noexcept {c4query_release(c);}
    static inline void releaseRef(C4QueryEnumerator* c)   noexcept {c4queryenum_release(c);}
    static inline void releaseRef(C4QueryObserver* c)     noexcept {c4queryobs_free(c);}
    static inline void releaseRef(C4RawDocument* c)       noexcept {c4raw_free(c);}
    static inline void releaseRef(C4ReadStream* c)        noexcept {c4stream_close(c);}
    static inline void releaseRef(C4Replicator* c)        noexcept {c4repl_free(c);}
    static inline void releaseRef(C4WriteStream* c)       noexcept {c4stream_closeWriter(c);}

    // The functions the ref<> template calls to retain a reference. (Not all types can be retained)
    static inline C4Cert*     retainRef(C4Cert* c)     noexcept {return c4cert_retain(c);}
    static inline C4Database* retainRef(C4Database* c) noexcept {return c4db_retain(c);}
    static inline C4Document* retainRef(C4Document* c) noexcept {return c4doc_retain(c);}
    static inline C4KeyPair*  retainRef(C4KeyPair* c)  noexcept {return c4keypair_retain(c);}
    static inline C4Query*    retainRef(C4Query* c)    noexcept {return c4query_retain(c);}
    static inline C4QueryEnumerator* retainRef(C4QueryEnumerator* c) noexcept {return c4queryenum_retain(c);}


    /** A simple little smart pointer that frees the C4 object when it leaves scope.

        NOTE that the constructor and assignment operator that take a T* assume they're being given
        a newly created reference (i.e. the return value from some C API function that creates a
        reference), so they don't retain it, but will release it when destructed or reassigned.
        If the reference is an existing one instead, you'll need to call retainRef() on it
        first so the retains and releases balance! */
    template <class T>
    class ref {
    public:
        ref() noexcept                          :_obj(nullptr) { }
        ref(T *t) noexcept                      :_obj(t) { }
        ref(ref &&r) noexcept                   :_obj(r._obj) {r._obj = nullptr;}
        ref(const ref &r) noexcept              :_obj(retainRef(r._obj)) { }
        ~ref() noexcept                         {releaseRef(_obj);}

        static ref retaining(T *t)               {return ref(retainRef(t));}

        operator T* () const noexcept FLPURE    {return _obj;}
        T* operator -> () const noexcept FLPURE {return _obj;}
        T* get() const noexcept FLPURE          {return _obj;}

        ref& operator=(T *t) noexcept           {if (_obj) releaseRef(_obj); _obj = t; return *this;}
        ref& operator=(ref &&r) noexcept        {_obj = r._obj; r._obj = nullptr; return *this;}
        ref& operator=(const ref &r) noexcept   {*this = retainRef(r._obj); return *this;}

    private:
        T* _obj;
    };

    /// Returns a description of a C4Error as a _temporary_ C string, for use in logging.
    #define c4error_descriptionStr(ERR)     alloc_slice(c4error_getDescription(ERR)).asString().c_str()
    
}
