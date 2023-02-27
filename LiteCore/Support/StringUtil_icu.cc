//
// StringUtil_icu.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#if LITECORE_USES_ICU

#    include "StringUtil.hh"
#    include "Logging.hh"
#    include "icu_shim.h"

namespace litecore {
    using namespace fleece;

    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        UErrorCode error = U_ZERO_ERROR;
        UCaseMap  *csm   = lc_ucasemap_open(nullptr, 0, &error);
        if ( _usuallyFalse(!U_SUCCESS(error)) ) { return {}; }

        alloc_slice result(str.size);
        int32_t     resultSize;
        bool        finished = false;
        while ( !finished ) {
            if ( toUppercase ) {
                resultSize = lc_ucasemap_utf8ToUpper(csm, (char *)result.buf, (int32_t)result.size,
                                                     (const char *)str.buf, (int32_t)str.size, &error);
            } else {
                resultSize = lc_ucasemap_utf8ToLower(csm, (char *)result.buf, (int32_t)result.size,
                                                     (const char *)str.buf, (int32_t)str.size, &error);
            }

            if ( _usuallyFalse(!U_SUCCESS(error) && error != U_BUFFER_OVERFLOW_ERROR) ) {
                lc_ucasemap_close(csm);
                return {};
            }

            if ( _usuallyFalse(resultSize != result.size) ) {
                result.resize(resultSize);
                finished = resultSize < result.size;
            } else {
                finished = true;
            }
        }

        lc_ucasemap_close(csm);
        return result;
    }
}  // namespace litecore
#endif