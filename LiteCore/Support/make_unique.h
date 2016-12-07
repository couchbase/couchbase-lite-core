//
//  make_unique.h
//  LiteCore
//
//  Created by Jens Alfke on 11/18/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Copied from http://src.couchbase.org/source/xref/trunk/platform/include/platform/make_unique.h


/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

//
// Backport of C++14's std::make_unique() to C++11
//
// Taken from llvm.org's libc++ implementation (dual licensed under
// the MIT license and the UIUC License (a BSD-like license):
// http://llvm.org/viewvc/llvm-project/libcxx/trunk/include/memory?r1=185352&r2=185351&pathrev=185352
//

#ifndef _MSC_VER
#pragma once

#if __cplusplus < 201402L

#include <memory>

namespace std
{
//    template<class T, class... Args> unique_ptr<T> make_unique(Args&&... args);     // C++14
//    template<class T>                unique_ptr<T> make_unique(size_t n);           // C++14
//}  // std


template<class _Tp>
struct __unique_if
{
    typedef std::unique_ptr<_Tp> __unique_single;
};

template<class _Tp>
struct __unique_if<_Tp[]>
{
    typedef std::unique_ptr<_Tp[]> __unique_array_unknown_bound;
};

template<class _Tp, size_t _Np>
struct __unique_if<_Tp[_Np]>
{
    typedef void __unique_array_known_bound;
};

template<class _Tp, class... _Args>
inline
typename __unique_if<_Tp>::__unique_single
make_unique(_Args&&... __args)
{
    return std::unique_ptr<_Tp>(new _Tp(std::forward<_Args>(__args)...));
}

template<class _Tp>
inline
typename __unique_if<_Tp>::__unique_array_unknown_bound
make_unique(size_t __n)
{
    typedef typename std::remove_extent<_Tp>::type _Up;
    return std::unique_ptr<_Tp>(new _Up[__n]());
}

} // end namespace std

#else
#warning make_unique.h not intended for C++14 upwards.
#endif  // __cplusplus < 201402L
#endif // !_MSC_VER
