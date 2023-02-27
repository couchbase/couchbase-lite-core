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

// Adapted from OpenBSD string.h (Copyright (c) 1990 The Regents of the University of California.)
// Originally licensed under the BSD 3-clause license
// https://github.com/openbsd/src/blob/1693b10bbdb876791a8ba7277dd26d2fd5e7aff8/include/string.h#L127-L128

#pragma once

#ifndef __cplusplus
#    include <stddef.h>
#else
#    include <cstddef>
extern "C" {
#endif
size_t strlcat(char *dst, const char *src, size_t dsize);

#ifdef __cplusplus
}
#endif
