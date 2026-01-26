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
#include "betterassert.hh"
#include <sqlite3.h>  // for sqlite3_temp_directory
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <mutex>
#include <random>
#include <fstream>

using namespace std;
using namespace fleece;
using namespace litecore;

namespace litecore {

    static filesystem::perms perms_from_mode(int mode) {
        filesystem::perms perms = filesystem::perms::none;
        if ( mode & 0400 ) perms |= filesystem::perms::owner_read;
        if ( mode & 0200 ) perms |= filesystem::perms::owner_write;
        if ( mode & 0100 ) perms |= filesystem::perms::owner_exec;
        if ( mode & 0040 ) perms |= filesystem::perms::group_read;
        if ( mode & 0020 ) perms |= filesystem::perms::group_write;
        if ( mode & 0010 ) perms |= filesystem::perms::group_exec;
        if ( mode & 0004 ) perms |= filesystem::perms::others_read;
        if ( mode & 0002 ) perms |= filesystem::perms::others_write;
        if ( mode & 0001 ) perms |= filesystem::perms::others_exec;
        return perms;
    }

    static int to_errno(const std::error_code &ec) {
        if (ec.category() == std::generic_category()) {
            return ec.value();
        }

        return ec.default_error_condition().value();
    }

    static std::string randomSuffix(size_t len = 12) {
        static const char kChars[] =
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
        std::string s;
        s.reserve(len);
        for (size_t i = 0; i < len; ++i) s.push_back(kChars[dist(rng)]);
        return s;
    }

#ifdef _MSC_VER
    const string                FilePath::kSeparator = "\\";
    static const char           kSeparatorChar       = '\\';
    static const char           kQuotedSeparatorChar = ':';
    static filesystem::path     kCurrentDir          = ".";
    typedef struct _stat64 lc_stat_t;
#else
    const string                FilePath::kSeparator = "/";
    static const char           kSeparatorChar       = '/';
    static const char           kQuotedSeparatorChar = ':';
    static filesystem::path     kCurrentDir          = ".";
    typedef struct stat lc_stat_t;
#endif

    FilePath::FilePath(std::filesystem::path&& path)
        : _path(std::move(path))
    {
        auto pathStr = _path.string();

        // Trim off ending separator unless it's root because otherwise it
        // messes with the existing parentDir() logic.
        if ( pathStr.size() > 1 && pathStr.ends_with(kSeparator) ) {
            _path = filesystem::path(pathStr.substr(0, pathStr.size() - 1));
        }
    }

    FilePath::FilePath(const std::filesystem::path& path)
        : _path(path)
    {
        auto pathStr = _path.string();

        // Trim off ending separator unless it's root because otherwise it
        // messes with the existing parentDir() logic.
        if ( pathStr.size() > 1 && pathStr.ends_with(kSeparator) ) {
            _path = filesystem::path(pathStr.substr(0, pathStr.size() - 1));
        }

    }

    FilePath::FilePath() : FilePath(kCurrentDir) {}

    FilePath::operator alloc_slice() const { return alloc_slice(_path); }

    pair<string, string> FilePath::splitPath(string_view path) {
        filesystem::path fsPath(path);
        if ( !fsPath.has_root_path() ) {
            return {kCurrentDir, string(path)};
        }

        if ( filesystem::is_directory(fsPath) ) {
            return {fsPath.string(), ""};
        }

        return {fsPath.parent_path().string(), fsPath.filename().string()};
    }

    pair<string, string> FilePath::splitExtension(const string& file) {
        filesystem::path fsPath(file);
        return {fsPath.stem().string(), fsPath.extension().string()};
    }

    string FilePath::sanitizedFileName(string name) {
        for ( auto& c : name ) {
            if ( c == kSeparatorChar ) c = kQuotedSeparatorChar;
        }
        return name;
    }

    static filesystem::path addExtension(const filesystem::path& name, const string& ext) {
        return (ext[0] == '.') ? name.filename().string() + ext : name.filename().string() + "." + ext;
    }

    FilePath FilePath::withExtension(const string& ext) const {
        auto name = unextendedName();
        if ( ext.empty() ) return filesystem::path(_path.parent_path()) / name;
        else
            return filesystem::path(_path.parent_path()) / addExtension(name, ext);
    }

    FilePath FilePath::withExtensionIfNone(const string& ext) const {
        if ( extension().empty() ) return addingExtension(ext);
        else
            return *this;
    }

    FilePath FilePath::addingExtension(const string& ext) const {
        if ( ext.empty() ) return *this;
        return filesystem::path(_path.parent_path()) / addExtension(_path.filename(), ext);
    }

    FilePath FilePath::appendingToName(const std::string& suffix) const {
        auto otherPath = _path;
        otherPath += suffix;
        return FilePath(std::move(otherPath));
    }

    FilePath FilePath::operator[](const string& name) const {
        assert_precondition(isDir());
        return FilePath(_path / name);
    }

    FilePath FilePath::fileNamed(const std::string& filename) const { 
        assert_precondition(isDir());
        auto retVal = _path / filename;
        if(filesystem::exists(retVal)) {
            assert_postcondition(!filesystem::is_directory(retVal));
        }
        return FilePath(std::move(retVal));
    }

    FilePath FilePath::subdirectoryNamed(const std::string& dirname) const { 
        assert_precondition(isDir());
        auto retVal = _path / dirname;
        if(filesystem::exists(retVal)) {
            assert_postcondition(filesystem::is_directory(retVal));
        }
        return FilePath(std::move(retVal));
    }

    FilePath FilePath::parentDir() const {
        auto parent = _path.parent_path();
        if ( parent == _path ) {
            return *this; // root directory
        }
        if ( parent.string().empty() ) {
            if(_path.string() == kCurrentDir) {
                error::_throw(error::POSIX, EINVAL);
            }

            // relative path with no parent
            return FilePath(kCurrentDir);
        }

        return FilePath(std::move(parent));
    }

    /* static */ FilePath FilePath::sharedTempDirectory(const string& location) {
        FilePath alternate(location);
        alternate = alternate.dir();

        // Hardcode tmp name so that a new directory doesn't get created every time
        alternate = alternate.subdirectoryNamed(".cblite");
        alternate.mkdir(0755);
        return alternate;
    }

#pragma mark - ENUMERATION:

    void FilePath::forEachMatch(function_ref<void(const FilePath&)> fn) const {
        auto dir = isDir() ? _path : _path.parent_path();
        for(const auto& entry : filesystem::directory_iterator(dir)) {
            if(entry.path().filename() == "." || entry.path().filename() == "..") continue;
            if ( !isDir() && entry.path().filename().string().find(_path.filename().string()) != 0 ) continue;
            fn(FilePath(dir / entry));
        }
    }

    void FilePath::forEachFile(function_ref<void(const FilePath&)> fn) const { 
        auto dir = isDir() ? _path : _path.parent_path();
        for(const auto& entry : filesystem::directory_iterator(dir)) {
            if(entry.path().filename() == "." || entry.path().filename() == "..") continue;
            fn(FilePath(dir / entry));
        }
    }

#pragma mark - OPERATIONS:

    void FilePath::mustExistAsDir() const {
        if ( !existsAsDir() ) error::_throw(error::POSIX, ENOTDIR);
    }

    bool FilePath::mkdir(int mode) const {
        if(filesystem::exists(_path)) {
            return true;
        }

        error_code ec;
        if(!filesystem::create_directory(_path, ec)) {
            error::_throw(error::POSIX, to_errno(ec));
        }

        filesystem::permissions(_path, perms_from_mode(mode), ec);
        if ( ec ) {
            error::_throw(error::POSIX, to_errno(ec));
        }

        return true;
    }

    static string makePathTemplate(const FilePath* fp) {
        string      path     = fp->path();
        return path + randomSuffix();
    }

    FilePath FilePath::mkTempFile(FILE** outHandle) const {
        for(int i = 0; i < numeric_limits<int>::max(); i++) {
            auto pathTemplate = makePathTemplate(this);
            if(filesystem::exists(pathTemplate)) {
                continue;
            }

            ofstream f(pathTemplate, ios::out | ios::binary | ios::trunc);
            if(!f) {
                error(error::POSIX, EIO, "Unable to create temporary file")._throw(1);
            }

            if(outHandle) {
                int fd = open(pathTemplate.c_str(), O_RDWR | O_TRUNC);
                if (fd < 0) {
                    error(error::POSIX, errno, "Unable to open temporary file")._throw(1);
                }

                *outHandle = fdopen(fd, "wb");
                if( !*outHandle ) {
                    close(fd);
                    error(error::POSIX, errno, "Unable to fdopen temporary file")._throw(1);
                }
            }

            return FilePath(pathTemplate);
        }

        error::_throw(error::POSIX, EEXIST);
    }

    FilePath FilePath::mkTempDir() const {
        for(int i = 0; i < numeric_limits<int>::max(); i++) {
            auto pathTemplate = makePathTemplate(this);
            if(filesystem::exists(pathTemplate)) {
                continue;
            }

            error_code ec;
            if(!filesystem::create_directory(pathTemplate, ec)) {
                error::_throw(error::POSIX, to_errno(ec));
            }

            return FilePath(pathTemplate);
        }

        error::_throw(error::POSIX, EEXIST);
    }

    bool FilePath::del() const {
        error_code ec;
        auto result = filesystem::remove(_path, ec);
        if(result) {
            return true;
        }

        int err = to_errno(ec);
        if ( err == 0 ) return false;

#ifdef _MSC_VER
        if ( errno == EACCES ) {
            setReadOnly(false);
            result = filesystem::remove(_path, ec);
            if ( result == 0 ) { return true; }
        }
#endif

        error(error::POSIX, to_errno(ec), "Couldn't delete file")._throw(1);
    }

    static void _delRecursive(const FilePath& path) {
        if ( path.isDir() ) {
            path.forEachFile([](const FilePath& f) { f.delRecursive(); });
        }
        path.del();
    }

    bool FilePath::delRecursive() const {
        if ( !exists() ) return false;
        _delRecursive(*this);
        return true;
    }

    void FilePath::copyTo(const string& to) const {
        filesystem::copy(_path, to, filesystem::copy_options::recursive);
    }

    void FilePath::moveTo(const string& to) const {
        error_code ec;
        if(filesystem::exists(to)) {
            filesystem::permissions(to, filesystem::perms::owner_write, filesystem::perm_options::add, ec);
            if( ec ) {
                error::_throw(error::POSIX, to_errno(ec));
            }
        }

        filesystem::rename(_path, to, ec);
        if( ec ) {
            error::_throw(error::POSIX, to_errno(ec));
        }
    }

    void FilePath::moveToReplacingDir(const FilePath& to, bool asyncCleanup) const {
#ifdef _MSC_VER
        bool overwriting = to.exists();
#else
        bool overwriting = to.existsAsDir();
#endif
        if ( !overwriting ) {
            // Simple case; can do an atomic move
            moveTo(to);
            return;
        }

        // Move the old item aside, to be deleted later:
        auto     parent = to.parentDir().path();
        FilePath trashDir(FilePath::sharedTempDirectory(parent)["CBL_Obsolete-"].mkTempDir());
        FilePath trashPath = trashDir[to.fileOrDirName()];
        to.moveTo(trashPath);

        try {
            // Move to the destination:
            moveTo(to);
        } catch ( ... ) {
            // Crap! Put the old item back and fail:
            trashPath.moveTo(to);
            throw;
        }

        // Finally delete the old item:
        if ( asyncCleanup ) {
            thread([=] {
                trashDir.delRecursive();
                Log("Finished async delete of replaced <%s>", trashPath.path().c_str());
            }).detach();
        } else {
            trashDir.delRecursive();
        }
    }

    void FilePath::setReadOnly(bool readOnly) const { 
        filesystem::perms p = readOnly ? filesystem::perms::owner_read : (filesystem::perms::owner_read | filesystem::perms::owner_write);
        error_code ec;
        filesystem::permissions(_path, p, ec);
        if ( ec ) {
            error::_throw(error::POSIX, to_errno(ec));
        }
    }


}  // namespace litecore
