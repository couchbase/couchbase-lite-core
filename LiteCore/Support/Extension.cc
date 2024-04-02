//
// Extension.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Extension.hh"
#include "Logging.hh"
#include "Defer.hh"
#include "FilePath.hh"
#include <algorithm>
using namespace std;
using namespace litecore;

typedef const char* (*version_func)();
typedef int (*version_number_func)();

#ifdef WIN32
#    include <windows.h>
#    define cbl_dlopen(path)                                                                                           \
        LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
#    define cbl_dlsym   GetProcAddress
#    define cbl_dlclose FreeLibrary
#else
#    include <dlfcn.h>
#    define FARPROC          void*
#    define HMODULE          void*
#    define cbl_dlopen(path) dlopen(path, RTLD_NOW)
#    define cbl_dlsym        dlsym
#    define cbl_dlclose      dlclose
#endif

// const char* to avoid needless literal string conversion
static FARPROC get_function(HMODULE libHandle, const std::string& lowerCaseExtName, const char* functionSuffix) {
    string funcName     = lowerCaseExtName + functionSuffix;
    auto   function_ptr = cbl_dlsym(libHandle, funcName.c_str());
    if ( !function_ptr ) { LogToAt(DBLog, Error, "Invalid extension, no function named %s", funcName.c_str()); }

    return function_ptr;
}

static string name_from_path(const string& extensionPath) {
    auto lastSlash = extensionPath.rfind(FilePath::kSeparator);
    if ( lastSlash == string::npos ) { return ""; }

    auto raw_name = extensionPath.substr(lastSlash + 1);
    if ( raw_name.substr(0, 3) == "lib" ) { return raw_name.substr(3); }

    return raw_name;
}

static HMODULE try_open_lib(const string& extensionPath) {
#ifdef WIN32
    // Windows funny business.  .dll is auto appended to a naked filename.
    // Appending just a dot will stop that behavior.  So this will search
    // in the reverse order as the others.
    static constexpr const char* file_extension = ".";
#elif defined(__APPLE__)
    static constexpr const char* file_extension = ".dylib";
#else
    static constexpr const char* file_extension = ".so";
#endif

    LogToAt(DBLog, Info, "Looking for extension at %s", extensionPath.c_str());
    HMODULE libHandle = cbl_dlopen(extensionPath.c_str());
    if ( libHandle ) {
        LogToAt(DBLog, Info, "\t...Found!");
        return libHandle;
    }

    string with_extension = extensionPath + file_extension;
    LogToAt(DBLog, Info, "Looking for extension at %s", with_extension.c_str());
    libHandle = cbl_dlopen(with_extension.c_str());
    if ( libHandle ) { LogToAt(DBLog, Info, "\t...Found!"); }

#ifdef WIN32
    if ( libHandle ) {
        // https://www.sqlite.org/forum/forumpost/de61859ee6
        LogToAt(DBLog, Warning,
                "Library found without extension on Windows, this will fail in SQLite unfortunately...");
    }
#endif

    return libHandle;
}

bool litecore::extension::check_extension_version(const string& extensionPath, int expectedVersion) {
    HMODULE libHandle = try_open_lib(extensionPath.c_str());
    if ( !libHandle ) {
        LogToAt(DBLog, Error, "Unable to open extension to check version");
        return false;
    }

    DEFER { cbl_dlclose(libHandle); };


    string extensionName = name_from_path(extensionPath);
    if ( extensionName.empty() ) {
        LogToAt(DBLog, Error, "Invalid path specified (no slash): %s", extensionPath.c_str());
        return false;
    }

    string lowerCaseExtName = extensionName;
    transform(lowerCaseExtName.begin(), lowerCaseExtName.end(), lowerCaseExtName.begin(),
              [](unsigned char c) { return tolower(c); });
    version_number_func version_number_f =
            (version_number_func)get_function(libHandle, lowerCaseExtName, "_version_number");
    if ( !version_number_f ) {
        LogToAt(DBLog, Error, "Invalid extension '%s' (missing version number function)", extensionName.c_str());
        return false;
    }

    version_func version_f = (version_func)get_function(libHandle, lowerCaseExtName, "_version");
    if ( !version_f ) {
        LogToAt(DBLog, Error, "Invalid extension '%s' (missing version function)", extensionName.c_str());
        return false;
    }

    int         majorVersion = version_number_f() / 1000000;
    const char* versionStr   = version_f();
    if ( majorVersion == expectedVersion ) {
        LogToAt(DBLog, Info, "Loaded extension '%s' version %s", extensionName.c_str(), versionStr);
        return true;
    }

    LogToAt(DBLog, Error, "Mismatched version (%s is not major version %d)\n", versionStr, expectedVersion);
    return false;
}