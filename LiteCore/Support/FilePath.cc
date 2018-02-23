//
// FilePath.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "FilePath.hh"
#include "Base.hh"
#include "Logging.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "PlatformIO.hh"
#include <fcntl.h>
#include <cerrno>
#include <dirent.h>
#include <cstdio>
#include <thread>
#include <algorithm>

#ifndef _MSC_VER
#include <unistd.h>
#include <sys/stat.h>
#if __APPLE__
#include <copyfile.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include "strlcat.h"
#include <sys/sendfile.h>
#endif
#else
#include <atlbase.h>
#include <atlconv.h>
#include <WinBase.h>
#include <direct.h>
#include <io.h>
#include "strlcat.h"
#include "mkstemp.h"
#include "mkdtemp.h"
#include <sqlite3.h>
#endif

using namespace std;
using namespace fleece;

#ifdef __linux__
static int copyfile(const char* from, const char* to)
{
    int read_fd, write_fd;
    off_t offset = 0;
    struct stat stat_buf;
    read_fd = open(from, O_RDONLY);
    if(read_fd < 0) {
        return read_fd;
    }
    
    if(fstat(read_fd, &stat_buf) < 0) {
        int e = errno;
        close(read_fd);
        errno = e;
        return -1;
    }
    
    write_fd = open(to, O_WRONLY | O_CREAT, stat_buf.st_mode);
    if(write_fd < 0) {
        int e = errno;
        close(read_fd);
        errno = e;
        return write_fd;
    }
    
    if(sendfile(write_fd, read_fd, &offset, stat_buf.st_size) < 0) {
        int e = errno;
        close(read_fd);
        close(write_fd);
        errno = e;
        return -1;
    }
    
    if(close(read_fd) < 0) {
        int e = errno;
        close(write_fd);
        errno = e;
        return -1;
    }
    
    if(close(write_fd) < 0) {
        return -1;
    }
    
    return 0;
}
#elif defined(_MSC_VER)
typedef HRESULT (WINAPI *CopyFileFunc)(_In_ PCWSTR, _In_ PCWSTR, _In_opt_  COPYFILE2_EXTENDED_PARAMETERS *);

static int copyfile(const char* from, const char* to)
{
    CA2WEX<256> wideFrom(from, CP_UTF8);
    CA2WEX<256> wideTo(to, CP_UTF8);
    int err = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE kernelLib = LoadLibraryA("kernel32.dll");
    CopyFileFunc cpFile2 = (CopyFileFunc)GetProcAddress(kernelLib, "CopyFile2");
    if (cpFile2 != nullptr) {
        err = cpFile2(wideFrom, wideTo, nullptr);
    }
    else {
        // Windows 7 doesn't have CopyFile2
        err = CopyFileW(wideFrom, wideTo, false);
    }
#else
    err = CopyFile2(wideFrom, wideTo, nullptr);
#endif

    if(err != S_OK) {
        return -1;
    }
    
    return 0;
}
#endif

namespace litecore {

#ifdef _MSC_VER
    const string FilePath::kSeparator = "\\";
    static const char  kSeparatorChar = '\\';
    static const char  kBackupSeparatorChar = '/';
    static const char  kQuotedSeparatorChar = ':';
    static const char* kCurrentDir = ".\\";
#else
    const string FilePath::kSeparator = "/";
    static const char  kSeparatorChar = '/';
    static const char  kBackupSeparatorChar = '\\';
    static const char  kQuotedSeparatorChar = ':';
    static const char* kCurrentDir = "./";
#endif


    FilePath::FilePath(const string &dirName, const string &fileName)
    :_dir(dirName), _file(fileName)
    {
        if (_dir.empty())
            _dir = kCurrentDir;
        else if (_dir[_dir.size() - 1] == kBackupSeparatorChar)
            _dir[_dir.size() - 1] = kSeparatorChar;
        else if (_dir[_dir.size()-1] != kSeparatorChar)
            _dir += kSeparatorChar;
    }


    FilePath::FilePath()
    :_dir(kCurrentDir), _file()
    { }


    pair<string,string> FilePath::splitPath(const string &path) {
        string dirname, basename;
        auto slash = path.rfind(kSeparatorChar);
        auto backupSlash = path.rfind(kBackupSeparatorChar);
        if (slash == string::npos && backupSlash == string::npos) {
            return{ kCurrentDir, path };
        }
        
        if (slash == string::npos) {
            slash = backupSlash;
        }
        else if (backupSlash != string::npos) {
            slash = std::max(slash, backupSlash);
        }

        return {path.substr(0, slash+1), path.substr(slash+1)};
    }

    pair<string,string> FilePath::splitExtension(const string &file) {
        auto dot = file.rfind('.');
        if (dot == string::npos)
            return {file, ""};
        else
            return {file.substr(0, dot), file.substr(dot)};
    }


    string FilePath::sanitizedFileName(string name) {
        for (auto &c : name) {
            if (c == kSeparatorChar)
                c = kQuotedSeparatorChar;
        }
        return name;
    }


    string FilePath::unextendedName() const {
        return splitExtension(fileOrDirName()).first;
    }

    
    string FilePath::extension() const {
        return splitExtension(fileOrDirName()).second;
    }


    static string addExtension(const string &name, const string &ext) {
        return (ext[0] == '.') ? name + ext : name + "." + ext;

    }


    FilePath FilePath::withExtension(const string &ext) const {
        Assert(!isDir());
        auto name = unextendedName();
        if (ext.empty())
            return FilePath(_dir, name);
        else
            return FilePath(_dir, addExtension(name, ext));
    }

    
    FilePath FilePath::withExtensionIfNone(const string &ext) const {
        if (extension().empty())
            return addingExtension(ext);
        else
            return *this;
    }

    
    FilePath FilePath::addingExtension(const string &ext) const {
        Assert(!isDir());
        if (ext.empty())
            return *this;
        else
            return FilePath(_dir, addExtension(_file, ext));
    }


    FilePath FilePath::appendingToName(const std::string &suffix) const {
        if (isDir())
            // Cut off the trailing slash, it will get added back in the constructor
            return FilePath(_dir.substr(0, _dir.size() - 1) + suffix, _file);
        else
            return FilePath(_dir, _file + suffix);
    }


    FilePath FilePath::operator[] (const string &name) const {
        Assert(isDir());
        if (name.empty())
            return *this;
        else if (name[name.size()-1] == kSeparatorChar || name[name.size() - 1] == kBackupSeparatorChar)
            return FilePath(_dir + name, "");
        else
            return FilePath(_dir, name);
    }


    string FilePath::fileOrDirName() const {
        if (!isDir())
            return fileName();
        // Remove the trailing separator from _dir:
        auto path = _dir;
        if (path.size() <= 1 || path == kCurrentDir)
            return "";
        chomp(path, kSeparatorChar);
        chomp(path, kBackupSeparatorChar);
        // Now return the last component:
        auto split = splitPath(path);
        return split.second;
    }


    FilePath FilePath::fileNamed(const std::string &filename) const {
        return FilePath(_dir, filename);
    }

    FilePath FilePath::subdirectoryNamed(const std::string &dirname) const {
        return FilePath(_dir + dirname, "");
    }


    FilePath FilePath::tempDirectory() {
#if !defined(_MSC_VER) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        const char *tmpDir = getenv("TMPDIR");
#else
        const char *tmpDir = sqlite3_temp_directory;
#endif

        if(tmpDir == nullptr) {
#ifdef _MSC_VER
            tmpDir = "C:\\tmp";
#else
            tmpDir = "/tmp";
#endif
        }
        return FilePath(tmpDir, "");
    }


#if __APPLE__
    template <class T>
    class autoreleasing {
    public:
        autoreleasing()         :_ref(nullptr) {}
        autoreleasing(T t)      :_ref(t) {}
        ~autoreleasing()        {if (_ref) CFRelease(_ref);}
        operator T () const     {return _ref;}
        T* operator& ()         {assert(!_ref); return &_ref;}
    private:
        T _ref;
    };

    static string _canonicalPath(CFURLRef url, CFErrorRef *outError) {
        autoreleasing<CFStringRef> cfCanonPath;
        if (!CFURLCopyResourcePropertyForKey(url, kCFURLCanonicalPathKey, &cfCanonPath, outError))
            return string();
        nsstring_slice canonSlice(cfCanonPath);
        string canon = canonSlice.asString();
        if (CFURLHasDirectoryPath(url))
            canon += kSeparatorChar;
        return canon;
    }
#endif


    string FilePath::canonicalPath() const {
#if __APPLE__
        autoreleasing<CFStringRef> cfpath = slice(path()).createCFString();
        autoreleasing<CFURLRef> url = CFURLCreateWithFileSystemPath(nullptr, cfpath, kCFURLPOSIXPathStyle, isDir());

        CFErrorRef error = nullptr;
        string canonPath = _canonicalPath(url, &error);
        if (!canonPath.empty())
            return canonPath;

        if (CFEqual(CFErrorGetDomain(error), kCFErrorDomainCocoa) && CFErrorGetCode(error) == 260) {
            // File doesn't exist, so get canonical path of parent directory:
            autoreleasing<CFURLRef> cfParentURL = CFURLCreateCopyDeletingLastPathComponent(nullptr, url);
            canonPath = _canonicalPath(cfParentURL, &error);
            if (!canonPath.empty()) {
                Assert(hasSuffix(canonPath, kSeparator));
                return canonPath + fileOrDirName();
            }
        }

        // Failure:
        Warn("canonicalPath(\"%s\") failed: CFError domain %s, code %ld",
             path().c_str(),
             alloc_slice(CFErrorGetDomain(error)).asString().c_str(), CFErrorGetCode(error));
#endif
        return path();
    }


#pragma mark - ENUMERATION:

    static bool is_dir(const struct dirent *entry, const string &basePath) {
        bool isDir = false;
#ifdef _DIRENT_HAVE_D_TYPE
        if(entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK) {
            isDir = (entry->d_type == DT_DIR);
        } else
#endif
        {
            struct stat stbuf;
            stat_u8((basePath + entry->d_name).c_str(), &stbuf);
            isDir = S_ISDIR(stbuf.st_mode);
        }

        return isDir;
    }

    void FilePath::forEachMatch(function_ref<void(const FilePath&)> fn) const {
        auto dir = opendir(_dir.c_str());
        if (!dir)
            error::_throwErrno();
        try {
            while (1) {
                struct dirent *result = readdir(dir);
                if (!result)
                    break;
                string name(result->d_name);
                if (_file.empty() || name.find(_file) == 0) {
                    if (is_dir(result, _dir)) {
                        if (name == "." || name == "..")
                            continue;
                        fn(FilePath(_dir + name + '/', ""));
                    } else {
                        fn(FilePath(_dir,name));
                    }
                }
            }
        } catch(...) {
            closedir(dir);
            throw;
        }
        closedir(dir);
    }


    void FilePath::forEachFile(function_ref<void(const FilePath&)> fn) const {
        dir().forEachMatch(fn);
    }


#pragma mark - OPERATIONS:


    static inline void check(int result) {
        if (_usuallyFalse(result != 0))
            error::_throwErrno();
    }

    
    int64_t FilePath::dataSize() const {
        struct stat s;
        if (stat_u8(path().c_str(), &s) != 0) {
            if (errno == ENOENT)
                return -1;
            error::_throwErrno();
        }
        return s.st_size;
    }

    bool FilePath::exists() const {
        struct stat s;
        return stat_u8(path().c_str(), &s) == 0;
    }

    bool FilePath::existsAsDir() const {
        struct stat s;
        return stat_u8(path().c_str(), &s) == 0 && S_ISDIR(s.st_mode);
    }

    void FilePath::mustExistAsDir() const {
        struct stat s;
        check(stat_u8(path().c_str(), &s));
        if (!S_ISDIR(s.st_mode))
            error::_throw(error::POSIX, ENOTDIR);
    }


    bool FilePath::mkdir(int mode) const {
        if (mkdir_u8(path().c_str(), mode) != 0) {
            if (errno != EEXIST)
                error::_throwErrno();
            return false;
        }
        return true;
    }


    static constexpr size_t kPathBufSize = 1024; // MAXPATHLEN

    static void makePathTemplate(const FilePath *fp, char *pathBuf) {
        string path = fp->path();
        const char *basePath = path.c_str();
        Assert(strlen(basePath) + 6 < kPathBufSize - 1);
        sprintf(pathBuf, "%sXXXXXX", basePath);
    }


    FilePath FilePath::mkTempFile(FILE* *outHandle) const {
        char pathBuf[kPathBufSize];
        makePathTemplate(this, pathBuf);
        int fd = mkstemp(pathBuf);
        if (fd < 0)
            error::_throwErrno();
        if (outHandle) {
            *outHandle = fdopen(fd, "wb+");
			if(*outHandle == nullptr) {
				fdclose(fd);
				error::_throwErrno();
			}
		} else {
            fdclose(fd);
		}

        return FilePath(pathBuf);
    }


    FilePath FilePath::mkTempDir() const {
        char pathBuf[kPathBufSize];
        makePathTemplate(this, pathBuf);
        if (mkdtemp(pathBuf) == nullptr)
            error::_throwErrno();
        
        static const char separator[2] = { kSeparatorChar, (char)0 };
        strlcat(pathBuf, separator, sizeof(pathBuf));
        return FilePath(pathBuf);
    }


    bool FilePath::del() const {
        auto result = isDir() ? rmdir_u8(path().c_str()) : unlink_u8(path().c_str());
        if (result == 0)
            return true;
        
        if (errno == ENOENT)
            return false;

#ifdef _MSC_VER
        if (errno == EACCES) {
            setReadOnly(false);
            result = isDir() ? rmdir_u8(path().c_str()) : unlink_u8(path().c_str());
            if (result == 0) {
                return true;
            }
        }
#endif

        error::_throwErrno();
    }

    static void _delRecursive(const FilePath &path) {
        if (path.isDir()) {
#if 1
            path.forEachFile([](const FilePath &f) {
                f.delRecursive();
            });
#else
            vector<FilePath> children;
            path.forEachFile([&](const FilePath &f) {
                children.push_back(f);
            });
            for (auto child : children)
                child.delRecursive();
#endif
        }
        path.del();
    }

    bool FilePath::delRecursive() const {
        if (!exists())
            return false;
        _delRecursive(*this);
        return true;
    }

    void FilePath::copyTo(const string &to) const {
#if __APPLE__
        copyfile_flags_t flags = COPYFILE_CLONE | COPYFILE_RECURSIVE;
        check(copyfile(path().c_str(), to.c_str(), nullptr, flags));
#else
        if (isDir()) {
            FilePath toPath(to);
            toPath.mkdir();
            forEachFile([&toPath](const FilePath &f) {
                f.copyTo(toPath[f.fileOrDirName() + (f.isDir() ? "/" : "")]);
            });
        } else {
            check(copyfile(path().c_str(), to.c_str()));
        }
#endif
    }

    void FilePath::moveTo(const string &to) const {
#ifdef _MSC_VER
        int result = chmod_u8(to.c_str(), 0600);
        if (result != 0) {
            if (errno != ENOENT) {
                error::_throwErrno();
            }
        } else {
            if ((FilePath(to).isDir())) {
                check(rmdir_u8(to.c_str()));
            }
            else {
                check(unlink_u8(to.c_str()));
            }
        }
#endif
        check(rename_u8(path().c_str(), to.c_str()));
    }


    void FilePath::moveToReplacingDir(const FilePath &to, bool asyncCleanup) const {
#ifdef _MSC_VER
        bool overwriting = to.exists();
#else
        bool overwriting = to.existsAsDir();
#endif
        if (!overwriting) {
            // Simple case; can do an atomic move
            moveTo(to);
            return;
        }

        // Move the old item aside, to be deleted later:
        FilePath trashDir(FilePath::tempDirectory()["CBL_Obsolete-"].mkTempDir());
        FilePath trashPath(trashDir, to.fileOrDirName());
        to.moveTo(trashPath);

        try {
            // Move to the destination:
            moveTo(to);
        } catch (...) {
            // Crap! Put the old item back and fail:
            trashPath.moveTo(to);
            throw;
        }

        // Finally delete the old item:
        if (asyncCleanup) {
            thread( [=]{
                trashDir.delRecursive();
                Log("Finished async delete of replaced <%s>", trashPath.path().c_str());
            } ).detach();
        } else {
            trashDir.delRecursive();
        }
    }


    void FilePath::setReadOnly(bool readOnly) const {
        chmod_u8(path().c_str(), (readOnly ? 0400 : 0600));
    }


}
