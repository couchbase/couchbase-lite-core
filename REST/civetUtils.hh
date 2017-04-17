//
//  civetUtils.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once

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
