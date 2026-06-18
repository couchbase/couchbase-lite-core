//
// atl_compat.hh
//
// Copyright 2026-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

// Drop-in, ATL-free replacements for the ATL string-conversion helpers
// CA2WEX / CW2AEX. ATL ships only with Visual Studio, so depending on it
// prevents building with a stand-alone clang/LLVM toolchain (e.g. clang-cl
// against an xwin-provided Windows SDK). These wrappers reproduce the small
// subset of the ATL API LiteCore uses, backed by the Win32 conversion APIs
// that live in the Windows SDK proper.

#include <windows.h>
#include <cstdlib>
#include <cwchar>

// CA2WEX<N>: convert a (UTF-8 by default) narrow string to wide (UTF-16).
// Uses an N-character stack buffer, falling back to the heap for long inputs.
template <int t_nBufferLength = 128>
class CA2WEX {
  public:
    explicit CA2WEX(const char* psz, UINT codePage = CP_UTF8) { init(psz, codePage); }

    ~CA2WEX() {
        if ( m_psz != m_buf ) free(m_psz);
    }

    CA2WEX(const CA2WEX&)            = delete;
    CA2WEX& operator=(const CA2WEX&) = delete;

    operator LPWSTR() const { return m_psz; }

    // ATL exposes m_psz publicly; some call sites read it directly.
    wchar_t* m_psz;

  private:
    void init(const char* psz, UINT codePage) {
        if ( psz == nullptr ) {
            m_psz = nullptr;
            return;
        }
        const int len = ::MultiByteToWideChar(codePage, 0, psz, -1, nullptr, 0);
        if ( len <= 0 ) {
            m_buf[0] = L'\0';
            m_psz    = m_buf;
            return;
        }
        m_psz = (len <= t_nBufferLength) ? m_buf : static_cast<wchar_t*>(malloc(len * sizeof(wchar_t)));
        ::MultiByteToWideChar(codePage, 0, psz, -1, m_psz, len);
    }

    wchar_t m_buf[t_nBufferLength];
};

// CW2AEX<N>: convert a wide (UTF-16) string to narrow (UTF-8 by default).
template <int t_nBufferLength = 128>
class CW2AEX {
  public:
    explicit CW2AEX(const wchar_t* psz, UINT codePage = CP_UTF8) { init(psz, codePage); }

    ~CW2AEX() {
        if ( m_psz != m_buf ) free(m_psz);
    }

    CW2AEX(const CW2AEX&)            = delete;
    CW2AEX& operator=(const CW2AEX&) = delete;

    operator LPSTR() const { return m_psz; }

    // ATL exposes m_psz publicly; some call sites read it directly.
    char* m_psz;

  private:
    void init(const wchar_t* psz, UINT codePage) {
        if ( psz == nullptr ) {
            m_psz = nullptr;
            return;
        }
        const int len = ::WideCharToMultiByte(codePage, 0, psz, -1, nullptr, 0, nullptr, nullptr);
        if ( len <= 0 ) {
            m_buf[0] = '\0';
            m_psz    = m_buf;
            return;
        }
        m_psz = (len <= t_nBufferLength) ? m_buf : static_cast<char*>(malloc(len));
        ::WideCharToMultiByte(codePage, 0, psz, -1, m_psz, len, nullptr, nullptr);
    }

    char m_buf[t_nBufferLength];
};

// Some call sites qualify the names with the ATL namespace.
namespace ATL {
    using ::CA2WEX;
    using ::CW2AEX;
}  // namespace ATL
