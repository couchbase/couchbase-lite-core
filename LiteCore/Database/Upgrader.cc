//
// Upgrader.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Base.hh"
#include "c4Document+Fleece.h"
#include "c4Collection.hh"
#include "Upgrader.hh"
#include "SQLite_Internal.hh"
#include "DatabaseImpl.hh"
#include "c4BlobStore.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "RevID.hh"
#include "FleeceImpl.hh"
#include "SQLiteCpp/Database.h"
#include "Stream.hh"
#include <sqlite3.h>
#include <memory>
#include <thread>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    static const int kMinOldUserVersion = 100;
    static const int kMaxOldUserVersion = 149;

    class Upgrader {
      public:
        Upgrader(const FilePath& oldPath, const FilePath& newPath, C4DatabaseConfig config)
            : Upgrader(oldPath, DatabaseImpl::open(newPath, asTreeVersioning(config))) {}

        Upgrader(const FilePath& oldPath, Retained<DatabaseImpl> newDB)
            : _oldPath(oldPath)
            , _oldDB(oldPath["db.sqlite3"].path(), SQLite::OPEN_READWRITE)  // *
            , _newDB(std::move(newDB))
            , _attachments(oldPath["attachments/"]) {
            // * Note: It would be preferable to open the old db read-only, but that will fail
            // unless its '-shm' file already exists. <https://www.sqlite.org/wal.html#readonly>

            sqlite3_create_collation(_oldDB.getHandle(), "REVID", SQLITE_UTF8, nullptr, &compareRevIDs);
        }

        static C4DatabaseConfig asTreeVersioning(C4DatabaseConfig config) {
            config.versioning = kC4TreeVersioning_v2;
            return config;
        }

        // Top-level method to invoke the upgrader.
        void run() {
            int userVersion = _oldDB.execAndGet("PRAGMA user_version");
            Log("SCHEMA UPGRADE: Upgrading CBL 1.x database <%s>, user_version=%d)", _oldPath.path().c_str(),
                userVersion);
            if ( userVersion < kMinOldUserVersion ) error::_throw(error::DatabaseTooOld);
            else if ( userVersion > kMaxOldUserVersion )
                error::_throw(error::CantUpgradeDatabase,
                              "Database cannot be upgraded because its internal version number isn't recognized");

            C4Database::Transaction t(_newDB);
            try {
                copyDocs();
#if 0
                copyLocalDocs();
#endif
                t.commit();
            } catch ( const std::exception& x ) {
                error       e    = error::convertException(x);
                const char* what = e.what();
                if ( !what ) what = "";
                error::_throw(error::CantUpgradeDatabase, "Error upgrading database: %s", what);
            }
        }


      private:
        static int compareRevIDs(C4UNUSED void* context, int len1, const void* chars1, int len2, const void* chars2) {
            revidBuffer rev1, rev2;
            rev1.parse({chars1, size_t(len1)});
            rev2.parse({chars2, size_t(len2)});
            if ( rev1.getRevID() < rev2.getRevID() ) return -1;
            else if ( rev1.getRevID() > rev2.getRevID() )
                return 1;
            else
                return 0;
        }

        // Copies all documents to the new db.
        void copyDocs() {
            SQLite::Statement allDocs(_oldDB, "SELECT doc_id, docid FROM docs");
            while ( allDocs.executeStep() ) {
                int64_t docKey = allDocs.getColumn(0);
                slice   docID  = getColumnAsSlice(allDocs, 1);

                if ( docID.hasPrefix("_"_sl) ) {
                    Warn("Skipping doc '%.*s': Document ID starting with an underscore is not permitted.",
                         SPLAT(docID));
                    continue;
                }

                Log("Importing doc '%.*s'", SPLAT(docID));
                try {
                    auto defaultColl = _newDB->getDefaultCollection();
                    auto newDoc      = defaultColl->getDocument(docID, false, kDocGetAll);
                    copyRevisions(docKey, newDoc);
                } catch ( const error& x ) {
                    // Add docID to exception message:
                    const char* what = x.what();
                    if ( !what ) what = "exception";
                    throw error(x.domain, x.code, stringprintf("%s, converting doc \"%.*s\"", what, SPLAT(docID)));
                }
            }
        }

        // Copies all revisions of a document.
        void copyRevisions(int64_t oldDocKey, C4Document* newDoc) {
            if ( !_currentRev ) {
                // Gets the current revision of doc
                _currentRev = std::make_unique<SQLite::Statement>(
                        _oldDB,
                        "SELECT sequence, revid, parent, deleted, json, no_attachments"
                        " FROM revs WHERE doc_id=? and current!=0"
                        " ORDER BY deleted, revid DESC LIMIT 1",
                        true);
                // Gets non-leaf revisions of doc in reverse sequence order
                _parentRevs = std::make_unique<SQLite::Statement>(
                        _oldDB,
                        "SELECT sequence, revid, parent, deleted, json, no_attachments"
                        " FROM revs WHERE doc_id=? and current=0"
                        " ORDER BY sequence DESC",
                        true);
            }

            _currentRev->reset();
            _currentRev->bind(1, (long long)oldDocKey);
            if ( !_currentRev->executeStep() ) return;  // huh, no revisions

            vector<alloc_slice> history;
            alloc_slice         revID(getColumnAsSlice(*_currentRev, 1));
            history.push_back(revID);
            Log("        ...rev %.*s", SPLAT(revID));

            // First row is the current revision:
            C4DocPutRequest put{};
            put.docID            = newDoc->docID();
            put.existingRevision = true;
            put.revFlags         = 0;
            if ( _currentRev->getColumn(3).getInt() != 0 ) put.revFlags = kRevDeleted;
            bool hasAttachments = _currentRev->getColumn(5).getInt() == 0;
            if ( hasAttachments ) put.revFlags |= kRevHasAttachments;

            // Convert the JSON body to Fleece:
            alloc_slice body;
            {
                Retained<Doc> doc = convertBody(getColumnAsSlice(*_currentRev, 4));
                if ( hasAttachments ) copyAttachments(doc);
                body = doc->allocedData();
            }
            put.allocedBody = {(void*)body.buf, body.size};

            int64_t nextSequence = _currentRev->getColumn(2);

            // Build the revision history:
            _parentRevs->reset();
            _parentRevs->bind(1, (long long)oldDocKey);
            while ( _parentRevs->executeStep() ) {
                if ( (int64_t)_parentRevs->getColumn(0) == nextSequence ) {
                    alloc_slice parentID(getColumnAsSlice(*_parentRevs, 1));
                    history.push_back(parentID);
                    Log("        ...rev %.*s", SPLAT(parentID));
                    nextSequence = _parentRevs->getColumn(2);
                }
            }

            put.historyCount = history.size();
            put.history      = (C4String*)history.data();
            put.save         = true;
            C4Error error;
            if ( !newDoc->putExistingRevision(put, &error) ) error::_throw((error::Domain)error.domain, error.code);
        }

        // Converts a JSON document body to Fleece.
        Retained<Doc> convertBody(slice json) {
            Encoder&      enc = _newDB->sharedEncoder();
            JSONConverter converter(enc);
            if ( !converter.encodeJSON(json) ) error::_throw(error::CorruptRevisionData, "invalid JSON data");
            return enc.finishDoc();
        }

        // Copies all blobs referenced in attachments of a revision from the old db.
        void copyAttachments(Doc* doc) {
            auto root = doc->asDict();
            if ( !root ) return;
            auto atts = root->get(C4STR(kC4LegacyAttachmentsProperty));
            if ( !atts ) return;
            auto attsDict = atts->asDict();
            if ( !attsDict ) return;
            for ( Dict::iterator i(attsDict); i; ++i ) {
                auto meta = i.value()->asDict();
                if ( meta ) {
                    auto digest = meta->get(slice(kC4BlobDigestProperty));
                    if ( digest ) copyAttachment(digest->asString());
                }
            }
        }

        // Copies a blob to the new database if it exists in the old one.
        bool copyAttachment(slice digest) {
            Log("        ...attachment '%.*s'", SPLAT(digest));
            optional<C4BlobKey> key = C4BlobKey::withDigestString(digest);
            if ( !key ) return false;
            string hex = slice(*key).hexString();
            for ( char& c : hex ) c = (char)toupper(c);
            FilePath src = _attachments[hex + ".blob"];
            if ( !src.exists() ) return false;

            //OPT: Could move the attachment file instead of copying (to save disk space)
            C4WriteStream  out(_newDB->getBlobStore());
            char           buf[32768];
            FileReadStream in(src);
            size_t         bytesRead;
            while ( 0 != (bytesRead = in.read(buf, sizeof(buf))) ) out.write({buf, bytesRead});
            out.install((C4BlobKey*)&key);
            return true;
        }

#if 0
        // Copies all "_local" documents to the new db.
        void copyLocalDocs() {
            SQLite::Statement localDocs(_oldDB, "SELECT docid, revid, json FROM localdocs");
            while (localDocs.executeStep()) {
                slice docID = asSlice(localDocs.getColumn(0));
                slice revID = asSlice(localDocs.getColumn(1));
                slice json  = asSlice(localDocs.getColumn(2));

                Log("Importing local doc '%.*s'", SPLAT(docID));
                Encoder enc;
                JSONConverter converter(enc);
                if (!converter.encodeJSON(json)) {
                    Warn("Upgrader: invalid JSON data in _local doc being upgraded; skipping");
                    continue;
                }
                auto body = enc.finish();
                _newDB->putRawDocument("_local", docID, revID, body);
            }
        }
#endif

        FilePath                      _oldPath;
        SQLite::Database              _oldDB;
        Retained<DatabaseImpl>        _newDB;
        FilePath                      _attachments;
        unique_ptr<SQLite::Statement> _currentRev, _parentRevs;
    };

    void UpgradeDatabase(const FilePath& oldPath, const FilePath& newPath, C4DatabaseConfig cfg) {
        Upgrader(oldPath, newPath, cfg).run();
    }

    bool UpgradeDatabaseInPlace(const FilePath& path, C4DatabaseConfig config) {
        if ( config.flags & (kC4DB_NoUpgrade | kC4DB_ReadOnly) ) return false;

        string p = path.path();
        chomp(p, '/');
        chomp(p, '\\');
        FilePath newTempPath(p + "_TEMP/");

        try {
            // Upgrade to a new db:
            auto newConfig = config;
            newConfig.flags |= kC4DB_Create;
            Log("Upgrader upgrading db <%s>; creating new db at <%s>", path.path().c_str(), newTempPath.path().c_str());
            UpgradeDatabase(path, newTempPath, newConfig);

            // Move the new db to the real path:
            newTempPath.moveToReplacingDir(path, true);
        } catch ( ... ) {
            newTempPath.delRecursive();
            throw;
        }

        Log("Upgrader finished");
        return true;
    }

}  // namespace litecore
