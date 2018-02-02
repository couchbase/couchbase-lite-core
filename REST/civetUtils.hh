//
// civetUtils.hh
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
#include <stdlib.h>
#include <time.h>
#include <string>

namespace litecore { namespace REST {

    void mg_strlcpy(register char *dst, register const char *src, size_t n);
    void gmt_time_string(char *buf, size_t buf_len, time_t *t);
    void urlDecode(const char *src,
                   size_t src_len,
                   std::string &dst,
                   bool is_form_url_encoded);
    void urlEncode(const char *src,
                   size_t src_len,
                   std::string &dst,
                   bool append);
    bool getParam(const char *data,
                  size_t data_len,
                  const char *name,
                  std::string &dst,
                  size_t occurrence);

} }
