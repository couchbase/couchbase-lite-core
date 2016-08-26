//
//  FilePath.cc
//  CBForest
//
//  Created by Jens Alfke on 8/19/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "FilePath.hh"
#include "LogInternal.hh"
#include "Error.hh"
#include <dirent.h>
#include <errno.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <sys/stat.h>
#endif


using namespace std;

namespace cbforest {

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
        CBFAssert(!isDir());
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
        CBFAssert(!isDir());
        if (ext.empty())
            return *this;
        else
            return FilePath(_dir, addExtension(_file, ext));
    }

    
    FilePath FilePath::operator[] (const string &name) const {
        CBFAssert(isDir());
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
            struct dirent entry, *result;
            while (1) {
                int err = readdir_r(dir, &entry, &result);
                if (err)
                    error::_throw(error::POSIX, err);
                else if (!result)
                    break;
                string name(result->d_name, result->d_namlen);
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
        if (result != 0)
            error::_throwErrno();
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
        if (::mkdir(path().c_str(), (mode_t)mode) != 0) {
            if (errno != EEXIST)
                error::_throwErrno();
            return false;
        }
        return true;
    }


    bool FilePath::del() const {
        //fprintf(stderr, "DEL %s\n", path().c_str());
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
            path.forEachFile([](const FilePath &f) {
                f.delRecursive();
            });
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

}
