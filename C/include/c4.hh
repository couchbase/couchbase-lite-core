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
#include "c4Base.h"
#include <utility>


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


    /** Smart pointer for C4 references, similar to Retained<>.

        NOTE that construction and assignment from a T* assumes they're being given
        a newly created reference (i.e. the return value from some C API function that creates a
        reference), so they _don't retain it_, but will release it when destructed or reassigned.
        If the reference is an existing one instead, call `retaining` on it first, so the retains
        and releases balance! */
    template <class T>
    class ref {
    public:
        constexpr ref() noexcept                :_obj(nullptr) { }
        constexpr ref(T *t) noexcept            :_obj(t) { }
        constexpr ref(ref &&r) noexcept         :_obj(r._obj) {r._obj = nullptr;}
        ref(const ref &r) noexcept              :_obj(retainRef(r._obj)) { }
        ~ref() noexcept                         {releaseRef(_obj);}

        static ref retaining(T *t)              {return ref(retainRef(t));}

        operator T* () const & noexcept FLPURE  {return _obj;}
        T* operator -> () const noexcept FLPURE {return _obj;}
        T* get() const noexcept FLPURE          {return _obj;}

        ref& operator=(std::nullptr_t) noexcept { replaceRef(nullptr); return *this; }
        ref& operator=(ref &&r) noexcept        { std::swap(_obj, r._obj); return *this;}
        ref& operator=(const ref &r) noexcept   { replaceRef(retainRef(r._obj)); return *this;}

        T* detach() && noexcept                 {auto o = _obj; _obj = nullptr; return o;}

        // This operator is dangerous enough that it's prohibited.
        // For details, see the lengthy comment in RefCounted.hh, around line 153.
        operator T* () const && =delete;

    private:
        inline void replaceRef(T* newRef) {
            if (_obj) releaseRef(_obj);
            _obj = newRef;
        }
        
        T* _obj;
    };


    /// Convenience function for wrapping a new C4 object in a ref:
    template <class T>
    ref<T> make_ref(T *t) { return ref<T>(t); }


    /// Returns a description of a C4Error as a _temporary_ C string, for use in logging.
#ifndef c4error_descriptionStr
    #define c4error_descriptionStr(ERR)     fleece::alloc_slice(c4error_getDescription(ERR)).asString().c_str()
#endif

}
