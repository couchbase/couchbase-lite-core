//
//  c4.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once

#ifndef __cplusplus
#error "This is C++ only"
#endif

#include "c4.h"


namespace c4 {

    static inline void freeRef(C4Database* c)          {c4db_free(c);}
    static inline void freeRef(C4RawDocument* c)       {c4raw_free(c);}
    static inline void freeRef(C4Document* c)          {c4doc_free(c);}
    static inline void freeRef(C4DocEnumerator* c)     {c4enum_free(c);}
    static inline void freeRef(C4ExpiryEnumerator* c)  {c4exp_free(c);}
    static inline void freeRef(C4DatabaseObserver* c)  {c4dbobs_free(c);}
    static inline void freeRef(C4DocumentObserver* c)  {c4docobs_free(c);}
    static inline void freeRef(C4Key* c)               {c4key_free(c);}
    static inline void freeRef(C4KeyValueList* c)      {c4kv_free(c);}
    static inline void freeRef(C4View* c)              {c4view_free(c);}
    static inline void freeRef(C4QueryEnumerator* c)   {c4queryenum_free(c);}
    static inline void freeRef(C4Query* c)             {c4query_free(c);}


    template <class T>
    class ref {
    public:
        ref()                       :_obj(nullptr) { }
        ref(T *t)                   :_obj(t) { }
        ref(ref &&r)                :_obj(r._obj) {r._obj = nullptr;}
        ~ref()                      {if (_obj) freeRef(_obj);}

        operator T* ()              {return _obj;}
        T* operator -> ()           {return _obj;}

        ref& operator=(T *t)        {if (_obj) freeRef(_obj); _obj = t; return *this;}

    private:
        ref(const ref&) =delete;
        ref& operator=(const ref&) =delete;

        T* _obj;
    };

}
