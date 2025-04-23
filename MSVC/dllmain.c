#pragma comment(lib, "Version.lib")

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "c4Log.h"

typedef struct {
    WORD major;
    WORD minor;
    WORD build;
    WORD revision;
} Version;

static int GetDllVersion(const wchar_t* path, Version* out_version) {
    DWORD dummy;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if ( size == 0 ) { return 0; }

    BYTE* buffer = (BYTE*)malloc(size);
    if ( !buffer ) { return 0; }
    if ( !GetFileVersionInfoW(path, 0, size, buffer) ) {
        c4log(kC4DefaultLog, kC4LogVerbose, "GetFileVersionInfoW failed: %lu", GetLastError());
        free(buffer);
        return 0;
    }

    VS_FIXEDFILEINFO* fileInfo;
    UINT              len = 0;
    if ( !VerQueryValueA(buffer, "\\", (LPVOID*)&fileInfo, &len) ) {
        c4log(kC4DefaultLog, kC4LogVerbose, "VerQueryValueA failed: %lu", GetLastError());
        free(buffer);
        return 0;
    }

    if ( fileInfo ) {
        out_version->major    = HIWORD(fileInfo->dwFileVersionMS);
        out_version->minor    = LOWORD(fileInfo->dwFileVersionMS);
        out_version->build    = HIWORD(fileInfo->dwFileVersionLS);
        out_version->revision = LOWORD(fileInfo->dwFileVersionLS);
        free(buffer);
        return 1;
    }

    c4log(kC4DefaultLog, kC4LogVerbose, "VerQueryValueA returned NULL");
    free(buffer);
    return 0;
}

static int CompareVersions(const Version* a, const Version* b) {
    if ( a->major != b->major ) return a->major < b->major ? -1 : 1;
    if ( a->minor != b->minor ) return a->minor < b->minor ? -1 : 1;
    if ( a->build != b->build ) return a->build < b->build ? -1 : 1;
    if ( a->revision != b->revision ) return a->revision < b->revision ? -1 : 1;
    return 0;
}

void CheckCppRuntime() {
    HMODULE hMod = GetModuleHandleA("msvcp140.dll");
    if ( !hMod ) { hMod = LoadLibraryA("msvcp140d.dll"); }

    if ( !hMod ) {
        c4log(kC4DefaultLog, kC4LogWarning, "msvcp140.dll not loaded yet, unable to check version...");
        return;
    }

    wchar_t path[MAX_PATH];
    if ( GetModuleFileNameW(hMod, path, MAX_PATH) == 0 ) {
        c4log(kC4DefaultLog, kC4LogWarning, "Unable to determine msvcp140.dll filename to check version...");
        return;
    }

    Version loaded = {0};
    if ( !GetDllVersion(path, &loaded) ) {
        c4log(kC4DefaultLog, kC4LogWarning, "Unable to get version of msvcp140.dll to check...");
        return;
    }

    Version expected = {14, 36, 32457, 0};
    int     cmp      = CompareVersions(&loaded, &expected);
    if ( cmp < 0 ) {
        c4log(kC4DefaultLog, kC4LogWarning, "msvcp140.dll version is older than expected: %u.%u.%u.%u < %u.%u.%u.%u",
              loaded.major, loaded.minor, loaded.revision, loaded.build, expected.major, expected.minor,
              expected.revision, expected.build);
        c4log(kC4DefaultLog, kC4LogWarning, "This may cause instability in your application");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch ( ul_reason_for_call ) {
        case DLL_PROCESS_ATTACH:
            CheckCppRuntime();
            break;
        default:
            break;
    }
    return TRUE;
}