//
// LiteCoreTool.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#pragma once
#include "Tool.hh"
#include "c4.h"

#if !defined(LITECORE_API_VERSION) || LITECORE_VERSION < 351
#   error "You are building with an old pre-3.0 version of LiteCore"
#endif

// Unofficial LiteCore helpers for using the C API in C++ code
#include "tests/c4CppUtils.hh"


static inline std::string to_string(C4String s) {
    return std::string((const char*)s.buf, s.size);
}

static inline C4Slice c4str(const std::string &s) {
    return {s.data(), s.size()};
}


class LiteCoreTool : public Tool {
public:
    static LiteCoreTool* instance() {return dynamic_cast<LiteCoreTool*>(Tool::instance);}

    explicit LiteCoreTool(const char* name);

    LiteCoreTool(const LiteCoreTool &parent)
    :Tool(parent)
    ,_db(c4::retainRef(parent._db))
    ,_dbFlags(parent._dbFlags)
    { }

    LiteCoreTool(const LiteCoreTool &parent, const char *commandLine)
    :Tool(parent, commandLine)
    ,_db(c4::retainRef(parent._db))
    ,_dbFlags(parent._dbFlags)
    { }

    ~LiteCoreTool();

    virtual void displayVersion();

    /// Reads initial flags like --writeable, --upgrade, --version
    void processDBFlags();

    static std::pair<std::string,std::string> splitDBPath(const std::string &path);
    static bool isDatabasePath(const std::string &path);
    static bool isDatabaseURL(const std::string&);

#ifdef COUCHBASE_ENTERPRISE
    struct TLSConfig : public C4TLSConfig {
        TLSConfig()         :C4TLSConfig{} { }
        c4::ref<C4Cert>     _certificate, _rootClientCerts;
        c4::ref<C4KeyPair>  _key;
    };

    static TLSConfig makeTLSConfig(std::string const& certFile, std::string const& keyFile,
                            std::string const& clientCertFile);
    static c4::ref<C4Cert> readCertFile(std::string const& certFile);
    static c4::ref<C4KeyPair> readKeyFile(std::string const& keyFile); // Note: May prompt for password

    /// passwordOrKey can be a password, or an AES256 key written as 64 hex digits.
    static bool setPasswordOrKey(C4EncryptionKey*, fleece::slice passwordOrKey);
#endif

    static void logError(std::string_view what, C4Error err);
    void errorOccurred(const std::string &what, C4Error err);

    [[noreturn]] static void fail(std::string_view what, C4Error err) {
        logError(what, err);
        fail();
    }

    [[noreturn]] static void fail() {Tool::fail();}
    [[noreturn]] static void fail(std::string_view message) {Tool::fail(message);}

protected:
    static constexpr C4Error kEncryptedDBError = {LiteCoreDomain, kC4ErrorNotADatabaseFile};

    static c4::ref<C4Database> openDatabase(std::string pathStr,
                                            C4DatabaseFlags,
                                            C4EncryptionKey const& = {});
    void openDatabase(std::string path, bool interactive);
    void openDatabaseFromURL(const std::string &url);

    c4::ref<C4Database>   _db;
    bool                  _shouldCloseDB {false};
    C4DatabaseFlags       _dbFlags {kC4DB_ReadOnly | kC4DB_NoUpgrade};
    bool                  _dbNeedsPassword {false};
};
