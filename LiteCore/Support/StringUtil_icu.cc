//
// StringUtil_icu.cc
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

#if LITECORE_USES_ICU

#include "StringUtil.hh"
#include "Logging.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include <unicode/ucasemap.h>
#include <unicode/urename.h>
#pragma clang diagnostic pop

namespace litecore {
    using namespace fleece;
    
    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        UErrorCode error = U_ZERO_ERROR;
        UCaseMap* csm = ucasemap_open(nullptr, 0, &error);
        if(_usuallyFalse(!U_SUCCESS(error))) {
            return{};
        }
        
        alloc_slice result(str.size);
        int32_t resultSize;
        bool finished = false;
        while(!finished) {
            if(toUppercase) {
                resultSize = ucasemap_utf8ToUpper(csm, (char *)result.buf, (int32_t)result.size, (const char *)str.buf, (int32_t)str.size, &error);
            } else {
                resultSize = ucasemap_utf8ToLower(csm, (char *)result.buf, (int32_t)result.size, (const char *)str.buf, (int32_t)str.size, &error);
            }
            
            if(_usuallyFalse(!U_SUCCESS(error) && error != U_BUFFER_OVERFLOW_ERROR)) {
                ucasemap_close(csm);
                return{};
            }
        
            if(_usuallyFalse(resultSize != result.size)) {
                result.resize(resultSize);
                finished = resultSize < result.size;
            } else {
                finished = true;
            }
        }
        
        ucasemap_close(csm);
        return result;
    }
}
#endif