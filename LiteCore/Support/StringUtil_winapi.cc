//
// StringUtil_winapi.cc
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

#ifdef _MSC_VER

#include "StringUtil.hh"
#include "Logging.hh"
#include "TempArray.hh"
#include <Windows.h>

namespace litecore {
    using namespace fleece;
    
    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        int length = MultiByteToWideChar(CP_UTF8, 0, (const char *)str.buf, str.size, nullptr, 0);
        if (length == 0) {
            return{};
        }

        TempArray(wstr, wchar_t, length);
        MultiByteToWideChar(CP_UTF8, 0, (const char *)str.buf, str.size, wstr, length);

        DWORD flags = toUppercase ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
        int resultLength = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wstr, length, nullptr, 0, nullptr, nullptr, 0);
        if (resultLength == 0) {
            return{};
        }

        TempArray(mapped, wchar_t, resultLength);
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wstr, length, mapped, resultLength, nullptr, nullptr, 0);

        int finalLength = WideCharToMultiByte(CP_UTF8, 0, mapped, resultLength, nullptr, 0, nullptr, nullptr);
        if (finalLength == 0) {
            return{};
        }

        alloc_slice result(finalLength);
        WideCharToMultiByte(CP_UTF8, 0, mapped, resultLength, (char *)result.buf, result.size, nullptr, nullptr);
        return result;
    }
}
#endif