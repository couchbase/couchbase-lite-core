//
// FilePath.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "FilePath.hh"
#include "Base.hh"
#include "Logging.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "PlatformIO.hh"
#include <sqlite3.h>            // for sqlite3_temp_directory
#include <fcntl.h>
#include <cerrno>
#include <dirent.h>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <mutex>

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
#endif

using namespace std;
using namespace fleece;
using namespace litecore;

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

    size_t expected = stat_buf.st_size;
    ssize_t bytes = 0;
    while (bytes < expected) {
        expected -= bytes;
        bytes = sendfile(write_fd, read_fd, &offset, expected);
        if (bytes < 0) {
            int e = errno;
            close(read_fd);
            close(write_fd);
            errno = e;
            return -1;
        } else if (bytes == 0) {
            // zero bytes are read. Do we want to try again? Well, let's consider it as an error
            Warn("sys/sendfile makes no progress copying %s to %s and we bail out as failure.", from, to);
            if (close(read_fd) < 0) {
                // take the first errno due to close.
                int e = errno;
                close(write_fd);
                errno = e;
            } else {
                close(write_fd);
            }
            return -1;
        }
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
    int err = CopyFile2(wideFrom, wideTo, nullptr);

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
    typedef struct _stat64 lc_stat_t;
#else
    const string FilePath::kSeparator = "/";
    static const char  kSeparatorChar = '/';
    static const char  kBackupSeparatorChar = '\\';
    static const char  kQuotedSeparatorChar = ':';
    static const char* kCurrentDir = "./";
    typedef struct stat lc_stat_t;
#endif


    static string& appendSeparatorTo(string &str) {
        if (str.empty() || str[str.size()-1] != kSeparatorChar)
            str += kSeparatorChar;
        return str;
    }


    FilePath::FilePath(string &&dirName, string &&fileName)
    :_dir(move(dirName)), _file(move(fileName))
    {
        if (_dir.empty())
            _dir = kCurrentDir;
        else if (_dir[_dir.size() - 1] == kBackupSeparatorChar)
            _dir[_dir.size() - 1] = kSeparatorChar;
        else
            appendSeparatorTo(_dir);
    }

    FilePath::FilePath(string_view dir, string_view file) :FilePath(string(dir), string(file)) { }
    FilePath::FilePath(const char *dir, const char *file) :FilePath(string(dir), string(file)) { }


    FilePath::FilePath()
    :_dir(kCurrentDir), _file()
    { }


    pair<string,string> FilePath::splitPath(string_view path) {
        string dirname, basename;
        auto slash = path.rfind(kSeparatorChar);
        auto backupSlash = path.rfind(kBackupSeparatorChar);
        if (slash == string::npos && backupSlash == string::npos) {
            return{ kCurrentDir, string(path) };
        }
        
        if (slash == string::npos) {
            slash = backupSlash;
        }
        else if (backupSlash != string::npos) {
            slash = std::max(slash, backupSlash);
        }

        return {string(path.substr(0, slash+1)), string(path.substr(slash+1))};
    }

    pair<string,string> FilePath::splitExtension(const string &file) {
        auto dot = file.rfind('.');
        auto lastSlash = file.rfind(kSeparatorChar);
        if (dot == string::npos || (lastSlash != string::npos && dot < lastSlash))
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


    FilePath::operator alloc_slice() const {
        auto dirSize = _dir.size(), fileSize = _file.size();
        alloc_slice result(dirSize + fileSize);
        memcpy((void*)result.offset(0),       _dir.data(),  dirSize);
        memcpy((void*)result.offset(dirSize), _file.data(), fileSize);
        return result;
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
    
    
    FilePath FilePath::parentDir() const {
        if (!isDir())
            return dir();
        
        auto p = path();
        
        if (p == kCurrentDir)
            error::_throw(error::POSIX, EINVAL);
#ifdef _MSC_VER
        else if (p.size() == 3 && p[1] == ':' && (p[2] == kSeparatorChar || p[2] == kBackupSeparatorChar))
            return *this;
#else
        else if (p.size() == 1 && (p[0] == kSeparatorChar || p[0] == kBackupSeparatorChar))
            return *this;
#endif
        
        chomp(p, kSeparatorChar);
        chomp(p, kBackupSeparatorChar);
        auto parentDir = splitPath(p).first;
        return FilePath(parentDir, "");
    }


    /* static */ FilePath FilePath::sharedTempDirectory(const string& location) {
        FilePath alternate(location);
        alternate = alternate.dir();
            
        // Hardcode tmp name so that a new directory doesn't get created every time
        alternate = alternate.subdirectoryNamed(".cblite");
        alternate.mkdir(0755);
        return alternate;
    }


    string FilePath::canonicalPath() const {
#ifdef _MSC_VER
        // Windows 10 has a new file path length limit of 32,767 chars (optionally)
        const auto wcanon = (wchar_t*)malloc(sizeof(wchar_t) * 32768);
        auto pathVal = path();
        const CA2WEX<256> wpath(pathVal.c_str(), CP_UTF8);
        const DWORD copied = GetFullPathNameW(wpath, 32768, wcanon, nullptr);
        wcanon[copied] = 0;
        char* canon = nullptr;
        if(copied == 0) {
            const DWORD err = GetLastError();
            if(err ==  ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                _set_errno(ENOENT);
            } else {
                _set_errno(EBADF);
            }
        } else {
            const CW2AEX<256> converted(wcanon, CP_UTF8);

            // Arbitrarily large to account for unforseen whacky UTF-8 names (4 bytes per wide char)
            canon = (char *)malloc(32767 * 4 + 1);
            strcpy_s(canon, 32767 * 4 + 1, converted.m_psz);            
        }

        free(wcanon);
#else
        char *canon = ::realpath(path().c_str(), nullptr);
#endif

        if (!canon) {
            if (errno == ENOENT && !isDir()) {
                string canonDir = dir().canonicalPath();
                return appendSeparatorTo(canonDir) + _file;
            } else {
                error::_throwErrno();
            }
        }
        string canonStr(canon);
        free(canon);
        return canonStr;
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
            lc_stat_t stbuf;
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
        lc_stat_t s;
        if (stat_u8(path().c_str(), &s) != 0) {
            if (errno == ENOENT)
                return -1;
            error::_throwErrno();
        }
        return s.st_size;
    }

    time_t FilePath::lastModified() const {
        lc_stat_t s;
        if (stat_u8(path().c_str(), &s) != 0) {
            if (errno == ENOENT)
                return -1;
            error::_throwErrno();
        }
        return s.st_mtime;
    }


    bool FilePath::exists() const noexcept {
        lc_stat_t s;
        return stat_u8(path().c_str(), &s) == 0;
    }

    bool FilePath::existsAsDir() const noexcept {
        lc_stat_t s;
        return stat_u8(path().c_str(), &s) == 0 && S_ISDIR(s.st_mode);
    }

    void FilePath::mustExistAsDir() const {
        lc_stat_t s;
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
        snprintf(pathBuf, kPathBufSize, "%sXXXXXX", basePath);
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
        std::string pathStr = path();
        const char *pathCStr = pathStr.c_str();

        auto result = isDir() ? rmdir_u8(pathCStr) : unlink_u8(pathCStr);
        if (result == 0)
            return true;
        
        if (errno == ENOENT)
            return false;

#ifdef _MSC_VER
        if (errno == EACCES) {
            setReadOnly(false);
            result = isDir() ? rmdir_u8(pathCStr) : unlink_u8(pathCStr);
            if (result == 0) {
                return true;
            }
        }
#endif

        error::_throwErrno("Couldn't delete file %s", pathCStr);
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
        std::string from = path();
        const char *fromPathStr = from.c_str(), *toPathStr = to.c_str();
        int result = 0;
#if __APPLE__
        copyfile_flags_t flags = COPYFILE_CLONE | COPYFILE_RECURSIVE;
        result = copyfile(fromPathStr, toPathStr, nullptr, flags);
#else
        if (isDir()) {
            FilePath toPath(to);
            toPath.mkdir();
            forEachFile([&toPath](const FilePath &f) {
                f.copyTo(toPath[f.fileOrDirName() + (f.isDir() ? "/" : "")]);
            });
        } else {
            result = copyfile(fromPathStr, toPathStr);
        }
#endif
        if (result != 0) {
            error::_throwErrno("Couldn't copy file from %s to %s", fromPathStr, toPathStr);
        }
    }

    void FilePath::moveTo(const string &to) const {
        int result = rename_u8(path().c_str(), to.c_str());
#ifdef _MSC_VER
        // While unix will automatically overwrite the target,
        // Windows will not
        int loopCount = 0;
        while(result != 0 && errno == EEXIST) {
            if(loopCount++ == 10) {
                WarnError("Unable to move blob after 10 attempts, giving up...");
                error::_throw(error::POSIX, EEXIST);
            }

            check(chmod_u8(to.c_str(), 0600));
            if ((FilePath(to).isDir())) {
                check(rmdir_u8(to.c_str()));
            }  else {
                check(unlink_u8(to.c_str()));
            }

            result = rename_u8(path().c_str(), to.c_str());
        }
#endif
        check(result);
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
        auto parent = to.parentDir().path();
        FilePath trashDir(FilePath::sharedTempDirectory(parent)["CBL_Obsolete-"].mkTempDir());
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
