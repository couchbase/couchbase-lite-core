//
// c4Base.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#ifndef __cplusplus
#error "This is C++ only"
#endif

#pragma once
#include "c4Base.h"
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include "fleece/slice.hh"
#include <exception>


/// Converts an exception thrown from LiteCore's C++ API into a `C4Error`.
C4Error C4ErrorFromException(const std::exception&) noexcept;


/// Returns a description of a C4Error as a _temporary_ C string, for use in logging.
#ifndef c4error_descriptionStr
    #define c4error_descriptionStr(ERR)     fleece::alloc_slice(c4error_getDescription(ERR)).asString().c_str()
#endif


// Just a mix-in that allows API class declarations to use common Fleece types un-namespaced:
struct C4Base {
    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;
    template <class T> using Retained = fleece::Retained<T>;
};
