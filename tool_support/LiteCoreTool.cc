//
// LiteCoreTool.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
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

#include "LiteCoreTool.hh"
#include "FilePath.hh"
#include "StringUtil.hh"            // for digittoint(), on non-BSD-like systems
#include <fleece/RefCounted.hh>

using namespace std;
using namespace litecore;


LiteCoreTool::LiteCoreTool(const char* name)
:Tool(name)
{
    if (const char* extPath = getenv("CBLITE_EXTENSION_PATH"))
        c4_setExtensionPath(slice(extPath));
}


LiteCoreTool::~LiteCoreTool() {
    if (_shouldCloseDB && _db) {
        C4Error err;
        bool ok = c4db_close(_db, &err);
        if (!ok)
            cerr << "Warning: error closing database: " << c4error_descriptionStr(err) << "\n";
    }
}


bool LiteCoreTool::isDatabasePath(const string &path) {
    return ! splitDBPath(path).second.empty();
}


pair<string,string> LiteCoreTool::splitDBPath(const string &pathStr) {
    FilePath path(pathStr);
    if (path.extension() != kC4DatabaseFilenameExtension)
        return {"",""};
    return std::make_pair(string(path.parentDir()), path.unextendedName());
}


bool LiteCoreTool::isDatabaseURL(const string &str) {
    C4Address addr;
    C4String dbName;
    return c4address_fromURL(slice(str), &addr, &dbName);
}


#if COUCHBASE_ENTERPRISE
static bool setHexKey(C4EncryptionKey *key, slice str) {
    if (str.size != 2 * kC4EncryptionKeySizeAES256)
        return false;
    uint8_t *dst = &key->bytes[0];
    for (size_t src = 0; src < 2 * kC4EncryptionKeySizeAES256; src += 2) {
        if (!isxdigit(str[src]) || !isxdigit(str[src+1]))
            return false;
        *dst++ = (uint8_t)(16*digittoint(str[src]) + digittoint(str[src+1]));
    }
    key->algorithm = kC4EncryptionAES256;
    return true;
}

bool LiteCoreTool::setPasswordOrKey(C4EncryptionKey *encryptionKey, slice passwordOrKey) {
    return setHexKey(encryptionKey, passwordOrKey)
        || c4key_setPassword(encryptionKey, slice(passwordOrKey), kC4EncryptionAES256);
}
#endif


void LiteCoreTool::displayVersion() {
    alloc_slice version = c4_getVersion();
    cout << "Couchbase Lite Core " << version << "\n";
    ::exit(0);
}


void LiteCoreTool::processDBFlags() {
    processFlags({
        {"--create",    [&]{_dbFlags |= kC4DB_Create; _dbFlags &= ~kC4DB_ReadOnly;}},
        {"--writeable", [&]{_dbFlags &= ~kC4DB_ReadOnly;}},
        {"--upgrade",   [&]{_dbFlags &= ~(kC4DB_NoUpgrade | kC4DB_ReadOnly);}},
#if LITECORE_API_VERSION >= 300
        {"--upgrade=vv",[&]{_dbFlags &= ~(kC4DB_NoUpgrade | kC4DB_ReadOnly);
            _dbFlags |= kC4DB_VersionVectors;}},
#endif
        {"--encrypted", [&]{_dbNeedsPassword = true;}},
        {"--version",   [&]{displayVersion();}},
        {"-v",          [&]{displayVersion();}},
    });
}


c4::ref<C4Database> LiteCoreTool::openDatabase(string pathStr,
                                               C4DatabaseFlags dbFlags,
                                               C4EncryptionKey const& key)
{
    fixUpPath(pathStr);
    auto [parentDir, dbName] = splitDBPath(pathStr);
    if (dbName.empty())
        fail("Database filename must have a '.cblite2' extension: " + pathStr);
    C4DatabaseConfig2 config = {slice(parentDir), dbFlags, key};
    C4Error err;
    c4::ref<C4Database> db = c4db_openNamed(slice(dbName), &config, &err);
    if (!db)
        fail(stringprintf("Couldn't open database %s", pathStr.c_str()), err);
    return db;
}


void LiteCoreTool::openDatabase(string pathStr, bool interactive) {
    assert(!_db);
    fixUpPath(pathStr);
    auto [parentDir, dbName] = splitDBPath(pathStr);
    if (dbName.empty())
        fail("Database filename must have a '.cblite2' extension: " + pathStr);
    C4DatabaseConfig2 config = {slice(parentDir), _dbFlags};
    C4Error err;
    if (!_dbNeedsPassword) {
        _db = c4db_openNamed(slice(dbName), &config, &err);
    } else {
        // If --encrypted flag given, skip opening db as unencrypted
        err = kEncryptedDBError;
    }

    while (!_db && err == kEncryptedDBError) {
#ifdef COUCHBASE_ENTERPRISE
        // Database is encrypted
        if (!interactive && !_dbNeedsPassword) {
            // Don't prompt for a password unless this is an interactive session
            fail("Database is encrypted (use `--encrypted` flag to get a password prompt)");
        }
        string prompt = "Password (or hex key) for database " + dbName + ":";
        if (config.encryptionKey.algorithm != kC4EncryptionNone)
            prompt = "Sorry, try again: ";
        string password = readPassword(prompt.c_str());
        if (password.empty())
            exit(1);
        if (!setPasswordOrKey(&config.encryptionKey, password)) {
            cout << "Error: Couldn't derive key from password\n";
            continue;
        }
        _db = c4db_openNamed(slice(dbName), &config, &err);
        if (!_db && err == kEncryptedDBError) {
            cout << "Failed to decrypt database using current method, trying old method..." << endl;
            if (!c4key_setPasswordSHA1(&config.encryptionKey, slice(password), kC4EncryptionAES256)) {
                cout << "Error: Couldn't derive key from password\n";
                continue;
            }

            _db = c4db_openNamed(slice(dbName), &config, &err);
        }
#else
        fail("Database is encrypted (Enterprise Edition is required to open encrypted databases)");
#endif
    }
    
    if (!_db) {
        if (err.domain == LiteCoreDomain && err.code == kC4ErrorCantUpgradeDatabase
                && (_dbFlags & kC4DB_NoUpgrade)) {
            fail("The database needs to be upgraded to be opened by this version of LiteCore.\n"
                 "**This will likely make it unreadable by earlier versions.**\n"
                 "To upgrade, add the `--upgrade` flag before the database path.\n"
                 "(Detailed error message", err);
        }
        fail(stringprintf("Couldn't open database %s", pathStr.c_str()), err);
    }
    _shouldCloseDB = true;
}


#ifdef COUCHBASE_ENTERPRISE
c4::ref<C4Cert> LiteCoreTool::readCertFile(string const& certFile) {
    alloc_slice certData = readFile(certFile);
    C4Error err;
    c4::ref<C4Cert> cert = c4cert_fromData(certData, &err);
    if (!cert)
        fail("Couldn't read X.509 certificate(s) from " + certFile, err);
    return cert;
}


c4::ref<C4KeyPair> LiteCoreTool::readKeyFile(string const& keyFile) {
    alloc_slice keyData = readFile(keyFile);
    alloc_slice keyPassword;
    if (keyData.containsBytes("-----BEGIN ENCRYPTED "_sl)) {
        string prompt = "Private key file " + keyFile + " is encrypted; what's the password? ";
        keyPassword = alloc_slice(readPassword(prompt.c_str()));
    }
    C4Error err;
    c4::ref<C4KeyPair> key = c4keypair_fromPrivateKeyData(keyData, keyPassword, &err);
    if (!key)
        fail("Couldn't parse or decrypt private key in file " + keyFile, err);
    return key;
}


LiteCoreTool::TLSConfig LiteCoreTool::makeTLSConfig(string const& certFile,
                                                    string const& keyFile,
                                                    string const& clientCertFile)
{
    TLSConfig tlsConfig{};

    if (certFile.find('/') == string::npos && certFile.find('\\') == string::npos) {
        // Interpret a string with no path delimiters as a cert name to be looked up in Keychain:
        C4Error error;
        tlsConfig._certificate = c4cert_load(slice(certFile), &error);
        if (!tlsConfig._certificate) {
            if (error == C4Error{LiteCoreDomain, kC4ErrorNotFound}) {
                fail("no certificate named '" + certFile
                     + "' found in secure store. (If this is a filename, put './' in front of it.)");
            } else if (error != C4Error{LiteCoreDomain, kC4ErrorUnimplemented}) {
                fail("failed to read '" + certFile + "' from secure certificate store", error);
            }
            // ... unless cert store is unimplemented; then treat it as a filename
        }
    }
    if (!tlsConfig._certificate)
        tlsConfig._certificate = readCertFile(certFile);

    if (!keyFile.empty()) {
        tlsConfig._key = readKeyFile(keyFile);
        tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromKey;
    } else {
        tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromCert;
    }

    if (!clientCertFile.empty()) {
        tlsConfig._rootClientCerts = readCertFile(clientCertFile);
        tlsConfig.requireClientCerts = true;
    }

    tlsConfig.certificate     = tlsConfig._certificate;
    tlsConfig.key             = tlsConfig._key;
    tlsConfig.rootClientCerts = tlsConfig._rootClientCerts;
    return tlsConfig;
}
#endif // COUCHBASE_ENTERPRISE


void LiteCoreTool::logError(string_view what, C4Error err) {
    std::cerr << "Error";
    if (!islower(what[0]))
        std::cerr << ":";
    std::cerr << " " << what;
    if (err.code) {
        fleece::alloc_slice message = c4error_getDescription(err);
        std::cerr << ": " << to_string(message);
    }
    std::cerr << "\n";
}

void LiteCoreTool::errorOccurred(const std::string &what, C4Error err) {
    logError(what, err);
    ++_errorCount;
    if (_failOnError)
        fail();
}
