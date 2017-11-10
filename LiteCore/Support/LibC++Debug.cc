//
//  LibC++Debug.cc
//  LiteCore
//
//  Created by Jens Alfke on 11/10/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#ifdef __APPLE__
#ifdef _LIBCPP_DEBUG

#include <__debug>

namespace std {
    // Resolves a link error building with libc++ in debug mode. Apparently this symbol would be in
    // the debug version of libc++.dylib, but we don't have that on Apple platforms.
    __1::__libcpp_debug_function_type __1::__libcpp_debug_function;
}

#endif
#endif
