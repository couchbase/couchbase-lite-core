//
//  FilePath.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/19/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "FilePath.hh"
#include "Base.hh"
#include "Logging.hh"
#include "Error.hh"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <sys/stat.h>
#else
#include <direct.h>
#include <io.h>
#include "mkstemps.h"
#endif


using namespace std;

namespace litecore {

#ifdef _MSC_VER
    static const char  kSeparatorChar = '\\';
    static const char  kQuotedSeparatorChar = ':';
    static const char* kCurrentDir = ".\\";
    static const char* kTempDir = "C:\\tmp\\";
#else
    static const char  kSeparatorChar = '/';
    static const char  kQuotedSeparatorChar = ':';
    static const char* kCurrentDir = "./";
    static const char* kTempDir = "/tmp/";
#endif


    FilePath::FilePath(const string &dirName, const string &fileName)
    :_dir(dirName), _file(fileName)
    {
        if (_dir.empty())
            _dir = kCurrentDir;
        else if (_dir[_dir.size()-1] != kSeparatorChar)
            _dir += kSeparatorChar;
    }


    FilePath::FilePath()
    :_dir(kCurrentDir), _file()
    { }


    pair<string,string> FilePath::splitPath(const string &path) {
        string dirname, basename;
        auto slash = path.rfind(kSeparatorChar);
        if (slash == string::npos)
            return {kCurrentDir, path};
        else
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


    bool FilePath::operator== (const FilePath &other) const {
        return _dir == other._dir && _file == other._file;
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
            return FilePath(_dir + suffix, _file);
        else
            return FilePath(_dir, _file + suffix);
    }


    FilePath FilePath::operator[] (const string &name) const {
        Assert(isDir());
        if (name.empty())
            return *this;
        else if (name[name.size()-1] == kSeparatorChar)
            return FilePath(_dir + name, "");
        else
            return FilePath(_dir, name);
    }


    FilePath FilePath::fileNamed(const std::string &filename) const {
        return FilePath(_dir, filename);
    }


    FilePath FilePath::tempDirectory() {
        return FilePath(kTempDir, "");
    }


#pragma mark - ENUMERATION:


    void FilePath::forEachMatch(function<void(const FilePath&)> fn) const {
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
                    if (result->d_type == DT_DIR) {
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


    void FilePath::forEachFile(function<void(const FilePath&)> fn) const {
        dir().forEachMatch(fn);
    }


#pragma mark - OPERATIONS:


    static inline void check(int result) {
        if (_usuallyFalse(result != 0))
            error::_throwErrno();
    }

    
    int64_t FilePath::dataSize() const {
        struct stat s;
        if (::stat(path().c_str(), &s) != 0) {
            if (errno == ENOENT)
                return -1;
            error::_throwErrno();
        }
        return s.st_size;
    }

    bool FilePath::exists() const {
        struct stat s;
        return ::stat(path().c_str(), &s) == 0;
    }

    bool FilePath::existsAsDir() const {
        struct stat s;
        return ::stat(path().c_str(), &s) == 0 && S_ISDIR(s.st_mode);
    }

    void FilePath::mustExistAsDir() const {
        struct stat s;
        check(::stat(path().c_str(), &s));
        if (!S_ISDIR(s.st_mode))
            error::_throw(error::POSIX, ENOTDIR);
    }


    bool FilePath::mkdir(int mode) const {
        if (MKDIR(path().c_str(), mode) != 0) {
            if (errno != EEXIST)
                error::_throwErrno();
            return false;
        }
        return true;
    }


    FilePath FilePath::mkTempFile(const string &suffix, FILE* *outHandle) const {
        char templ[1024]; // MAXPATHLEN
        sprintf(templ, "%sXXXXXX%s", path().c_str(), suffix.c_str());
        int fd = mkstemps(templ, (int)suffix.length());
        if (fd < 0)
            error::_throwErrno();
        if (outHandle)
            *outHandle = fdopen(fd, "w");
        else
            close(fd);
        return FilePath(templ);
    }


    bool FilePath::del() const {
        auto result = isDir() ? ::rmdir(path().c_str()) : unlink(path().c_str());
        if (result == 0)
            return true;
        if (errno == ENOENT)
            return false;
        error::_throwErrno();
    }

    bool FilePath::delWithAllExtensions() const {
        bool deleted = del();
        FilePath(_dir, _file + ".").forEachMatch([&](const FilePath &f) {
            if (f.del())
                deleted = true;
        });
        return deleted;
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

    
    void FilePath::moveTo(const string &to) const {
        check(::rename(path().c_str(), to.c_str()));
    }


    void FilePath::setReadOnly(bool readOnly) const {
        chmod(path().c_str(), (readOnly ? 0400 : 0600));
    }


}
