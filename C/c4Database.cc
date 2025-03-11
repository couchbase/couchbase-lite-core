//
// c4Database.cc
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Database.hh"
#include "c4Collection.hh"
#include "c4Query.hh"
#include "c4ExceptionUtils.hh"

#include "DatabaseImpl.hh"
#include "DatabaseCookies.hh"
#include "PrebuiltCopier.hh"
#include "SQLiteDataFile.hh"

#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "SecureSymmetricCrypto.hh"
#include "slice_stream.hh"
#include <cinttypes>
#include <optional>


// NOTE: Most of C4Database is implemented in its concrete subclass DatabaseImpl.


using namespace std;
using namespace fleece;
using namespace litecore;


CBL_CORE_API_IMPL const char* const kC4DatabaseFilenameExtension = ".cblite2";

CBL_CORE_API_IMPL C4StorageEngine const kC4SQLiteStorageEngine = "SQLite";


FLPURE static int _digittoint(char ch) noexcept {
    int d = ch - '0';
    if ((unsigned) d < 10)
        return d;
    d = ch - 'a';
    if ((unsigned) d < 6)
        return d + 10;
    d = ch - 'A';
    if ((unsigned) d < 6)
        return d + 10;
    return -1;
}

std::string C4UUID::to_string() const {
    string str = slice(this, sizeof(C4UUID)).hexString();
    for (size_t pos = 20; pos >= 8; pos -= 4)
        str.insert(pos, "-");
    return str;
}

bool C4UUID::parse(std::string_view str, C4UUID* out) noexcept {
    slice_istream in(str);
    size_t dst = 0;
    while (dst < sizeof(C4UUID)) {
        if (in.eof())
            return false;
        uint8_t c = in.readByte();
        if (int digit1 = _digittoint(c); digit1 >= 0) {
            if (int digit2 = _digittoint(in.readByte()); digit2 >= 0)
                out->bytes[dst++] = uint8_t((digit1 << 4) | digit2);
            else
                return false;
        } else if (c != '-') {
            return false;
        }
    }
    return true;
}



C4EncryptionKey C4EncryptionKeyFromPassword(slice password, C4EncryptionAlgorithm alg) {
    C4EncryptionKey key;
    AssertParam(password.size > 0, "Password is empty");
    AssertParam(alg == kC4EncryptionAES256, "Invalid encryption algorithm");
    if ( !litecore::DeriveKeyFromPassword(password, key.bytes, kEncryptionKeySize[alg]) )
        C4Error::raise(LiteCoreDomain, kC4ErrorCrypto, "Key derivation failed");
    key.algorithm = alg;
    return key;
}

C4EncryptionKey C4EncryptionKeyFromPasswordSHA1(slice password, C4EncryptionAlgorithm alg) {
    C4EncryptionKey key;
    AssertParam(password.size > 0, "Password is empty");
    AssertParam(alg == kC4EncryptionAES256, "Invalid encryption algorithm");
    if ( !litecore::DeriveKeyFromPasswordSHA1(password, key.bytes, kEncryptionKeySize[alg]) )
        C4Error::raise(LiteCoreDomain, kC4ErrorCrypto, "Key derivation failed");
    key.algorithm = alg;
    return key;
}

void C4Database::setExtensionPath(slice path) { SQLiteDataFile::setExtensionPath(string(path)); }

void C4Database::enableExtension(slice name, slice path) {
    SQLiteDataFile::enableExtension(string(name), string(path));
}

#pragma mark - STATIC LIFECYCLE METHODS:

static FilePath dbPath(slice name, slice parentDir) {
    if ( name.size == 0 || parentDir.size == 0 ) C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter);
    return FilePath(string(parentDir), string(name)).addingExtension(kC4DatabaseFilenameExtension);
}

static void ensureConfigDirExists(const C4DatabaseConfig2& config) {
    if ( !(config.flags & kC4DB_ReadOnly) ) (void)FilePath(slice(config.parentDirectory), "").mkdir();
}

static C4DatabaseConfig newToOldConfig(const C4DatabaseConfig2& config2) {
    return C4DatabaseConfig{config2.flags | kC4DB_AutoCompact, NULL,
                            (config2.flags & kC4DB_VersionVectors) ? kC4VectorVersioning : kC4TreeVersioning,
                            config2.encryptionKey};
}

/*static*/ bool C4Database::deleteDatabaseFileAtPath(const string& dbPath, C4StorageEngine storageEngine) {
    FilePath           path(dbPath);
    DataFile::Factory* factory;
    if ( storageEngine ) {
        factory = DataFile::factoryNamed(storageEngine);
        if ( !factory ) Warn("c4db_deleteNamed: unknown storage engine '%s'", storageEngine);
    } else {
        factory = DataFile::factoryForFile(path);
    }
    if ( !factory ) error::_throw(error::WrongFormat);
    return factory->deleteFile(path);
}

/*static*/ bool C4Database::deleteAtPath(slice dbPath) {
    // Find the db file in the bundle:
    FilePath bundle{dbPath, ""};
    if ( bundle.exists() ) {
        try {
            C4StorageEngine storageEngine = nullptr;
            auto dbFilePath = DatabaseImpl::findOrCreateBundle(std::string(bundle.dir()), false, storageEngine);
            // Delete it:
            deleteDatabaseFileAtPath(std::string(dbFilePath), storageEngine);
        } catch ( const error& x ) {
            if ( x.code != error::WrongFormat )  // ignore exception if db file isn't found
                throw;
        }
    }
    // Delete the rest of the bundle:
    return bundle.delRecursive();
}

/*static*/ bool C4Database::exists(slice name, slice inDirectory) { return dbPath(name, inDirectory).exists(); }

/*static*/ Retained<C4Database> C4Database::openNamed(slice name, const Config& config) {
    ensureConfigDirExists(config);
    FilePath         path      = dbPath(name, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    return DatabaseImpl::open(path, oldConfig);
}

/*static*/ Retained<C4Database> C4Database::openAtPath(slice path, C4DatabaseFlags flags, const C4EncryptionKey* key) {
    C4DatabaseConfig config = {flags};
    if ( key ) config.encryptionKey = *key;
    return DatabaseImpl::open(FilePath(path, ""), config);
}

/*static*/ void C4Database::copyNamed(slice sourcePath, slice destinationName, const Config& config) {
    ensureConfigDirExists(config);
    FilePath         from(sourcePath, "");
    FilePath         to        = dbPath(destinationName, config.parentDirectory);
    C4DatabaseConfig oldConfig = newToOldConfig(config);
    CopyPrebuiltDB(from, to, &oldConfig);
}

/*static*/ void C4Database::copyFileToPath(slice sourcePath, slice destinationPath, const C4DatabaseConfig& config) {
    return CopyPrebuiltDB(FilePath(sourcePath), FilePath(destinationPath), &config);
}

/*static*/ bool C4Database::deleteNamed(slice name, slice inDirectory) {
    // Split this into a variable to workaround a GCC 8 issue with constructor resolution
    auto path = alloc_slice(dbPath(name, inDirectory));
    return deleteAtPath(path);
}

/*static*/ void C4Database::shutdownLiteCore() { SQLiteDataFile::shutdown(); }

Retained<C4Database> C4Database::openAgain() const {
    auto config = _config;
    config.flags |= kC4DB_NoHousekeeping;
    return openNamed(getName(), config);
}

C4Collection* C4Database::getDefaultCollection() const {
    // Make a distinction: If the DB is open and the default collection is deleted
    // then simply return null.  If the DB is closed, an error should occur.
    checkOpen();
    return (_defaultCollection && !_defaultCollection->isValid()) ? nullptr : _defaultCollection;
}

C4Collection* C4Database::getDefaultCollectionSafe() const {
    C4Collection* dc = getDefaultCollection();
    if ( !dc ) { error::_throw(error::NotOpen); }
    return dc;
}

C4Database::C4Database(std::string name, std::string dir, const C4DatabaseConfig& inConfig)
    : _name(std::move(name))
    , _parentDirectory(std::move(dir))
    , _config{slice(_parentDirectory), inConfig.flags, inConfig.encryptionKey}
    , _configV1(inConfig) {}

#pragma mark - QUERIES:

Retained<C4Query> C4Database::newQuery(C4QueryLanguage language, slice expr, int* errPos) const {
    return C4Query::newQuery(getDefaultCollectionSafe(), language, expr, errPos);
}

#pragma mark - COOKIES:

alloc_slice C4Database::getCookies(const C4Address& request) {
    checkOpen();

    litecore::repl::DatabaseCookies cookies(this);
    string                          result = cookies.cookiesForRequest(request);
    if ( result.empty() ) return {};
    return alloc_slice(result);
}

bool C4Database::setCookie(slice setCookieHeader, slice fromHost, slice fromPath, bool acceptParentDomain) {
    checkOpen();

    litecore::repl::DatabaseCookies cookies(this);
    bool                            ok =
            cookies.setCookie(setCookieHeader.asString(), fromHost.asString(), fromPath.asString(), acceptParentDomain);
    if ( ok ) cookies.saveChanges();
    return ok;
}

void C4Database::clearCookies() {
    checkOpen();

    litecore::repl::DatabaseCookies cookies(this);
    cookies.clearCookies();
    cookies.saveChanges();
}

#pragma mark - COLLECTIONS:

void C4Database::forEachCollection(slice inScope, const CollectionSpecCallback& cb) const {
    if ( !inScope ) inScope = kC4DefaultScopeID;
    forEachCollection([&](const CollectionSpec& spec) {
        if ( spec.scope == inScope ) cb(spec);
    });
}
