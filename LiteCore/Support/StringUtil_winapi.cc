//
// StringUtil_winapi.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef _MSC_VER

#    include "StringUtil.hh"
#    include "Logging.hh"
#    include "TempArray.hh"
#    include "NumConversion.hh"
#    include <Windows.h>

namespace litecore {
    using namespace fleece;

    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        int length = MultiByteToWideChar(CP_UTF8, 0, (const char*)str.buf, narrow_cast<DWORD>(str.size), nullptr, 0);
        if ( length == 0 ) { return {}; }

        TempArray(wstr, wchar_t, length);
        MultiByteToWideChar(CP_UTF8, 0, (const char*)str.buf, narrow_cast<DWORD>(str.size), wstr, length);

        DWORD flags = toUppercase ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
        int   resultLength
                = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wstr, length, nullptr, 0, nullptr, nullptr, 0);
        if ( resultLength == 0 ) { return {}; }

        TempArray(mapped, wchar_t, resultLength);
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, wstr, length, mapped, resultLength, nullptr, nullptr, 0);

        int finalLength = WideCharToMultiByte(CP_UTF8, 0, mapped, resultLength, nullptr, 0, nullptr, nullptr);
        if ( finalLength == 0 ) { return {}; }

        alloc_slice result(finalLength);
        WideCharToMultiByte(CP_UTF8, 0, mapped, resultLength, (char*)result.buf, narrow_cast<DWORD>(result.size),
                            nullptr, nullptr);
        return result;
    }
}  // namespace litecore
#endif