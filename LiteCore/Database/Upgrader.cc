//
// Upgrader.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "c4Internal.hh"
#include "c4Document+Fleece.h"
#include "Upgrader.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Database.hh"
#include "Document.hh"
#include "BlobStore.hh"
#include "FleeceImpl.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "RevID.hh"
#include <sqlite3.h>
#include <thread>

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace c4Internal;


namespace litecore {

    static const int kMinOldUserVersion = 100;
    static const int kMaxOldUserVersion = 149;

    class Upgrader {
    public:
        Upgrader(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig config)
        :Upgrader(oldPath, new Database(newPath.path(), config))
        { }


        Upgrader(const FilePath &oldPath, Database *newDB)
        :_oldPath(oldPath)
        ,_oldDB(oldPath["db.sqlite3"].path(), SQLite::OPEN_READWRITE) // *
        ,_newDB(newDB)
        ,_attachments(oldPath["attachments/"])
        {
            // * Note: It would be preferable to open the old db read-only, but that will fail
            // unless its '-shm' file already exists. <https://www.sqlite.org/wal.html#readonly>

            sqlite3_create_collation(_oldDB.getHandle(), "REVID", SQLITE_UTF8, NULL,
                                     &compareRevIDs);
        }


        // Top-level method to invoke the upgrader.
        void run() {
            int userVersion = _oldDB.execAndGet("PRAGMA user_version");
            Log("Upgrading CBL 1.x database <%s>, user_version=%d)",
                _oldPath.path().c_str(), userVersion);
            if (userVersion < kMinOldUserVersion)
                error::_throw(error::DatabaseTooOld);
            else if (userVersion > kMaxOldUserVersion)
                error::_throw(error::CantUpgradeDatabase);

            _newDB->beginTransaction();
            try {
                copyDocs();
#if 0
                copyLocalDocs();
#endif
            } catch (const std::exception &x) {
                _newDB->endTransaction(false);
                error e = error::convertException(x);
                const char *what = e.what();
                if (!what) what = "";
                error::_throw(error::CantUpgradeDatabase, "Error upgrading database: %s", what);
            }
            _newDB->endTransaction(true);
        }


    private:

        static int compareRevIDs(void *context, int len1, const void * chars1,
                                                int len2, const void * chars2)
        {
            revidBuffer rev1, rev2;
            rev1.parse({chars1, size_t(len1)});
            rev2.parse({chars2, size_t(len2)});
            if (rev1 < rev2)
                return -1;
            else if (rev1 > rev2)
                return 1;
            else
                return 0;
        }


        static inline slice asSlice(const SQLite::Column &col) {
            return slice(col.getBlob(), col.size());
        }


        // Copies all documents to the new db.
        void copyDocs() {
            SQLite::Statement allDocs(_oldDB, "SELECT doc_id, docid FROM docs");
            while (allDocs.executeStep()) {
                int64_t docKey = allDocs.getColumn(0);
                slice docID = asSlice(allDocs.getColumn(1));

                if (docID.hasPrefix("_"_sl)) {
                    Warn("Skipping doc '%.*s': Document ID starting with an underscore is not permitted.", SPLAT(docID));
                    continue;
                }

                Log("Importing doc '%.*s'", SPLAT(docID));
                try {
                    Retained<Document> newDoc(
                                    _newDB->documentFactory().newDocumentInstance(docID));
                    copyRevisions(docKey, newDoc);
                } catch (const error &x) {
                    // Add docID to exception message:
                    const char *what = x.what();
                    if (!what)
                        what = "exception";
                    throw error(x.domain, x.code,
                                format("%s, converting doc \"%.*s\"", what, SPLAT(docID)).c_str());
                }
            }
        }


        // Copies all revisions of a document.
        void copyRevisions(int64_t oldDocKey, Document *newDoc) {
            if (!_currentRev) {
                // Gets the current revision of doc
                _currentRev.reset(new SQLite::Statement(_oldDB,
                                 "SELECT sequence, revid, parent, deleted, json, no_attachments"
                                 " FROM revs WHERE doc_id=? and current!=0"
                                 " ORDER BY deleted, revid DESC LIMIT 1"));
                // Gets non-leaf revisions of doc in reverse sequence order
                _parentRevs.reset(new SQLite::Statement(_oldDB,
                                 "SELECT sequence, revid, parent, deleted, json, no_attachments"
                                 " FROM revs WHERE doc_id=? and current=0"
                                 " ORDER BY sequence DESC"));
            }

            _currentRev->reset();
            _currentRev->bind(1, (long long)oldDocKey);
            if (!_currentRev->executeStep())
                return;     // huh, no revisions

            vector<alloc_slice> history;
            alloc_slice revID(asSlice(_currentRev->getColumn(1)));
            history.push_back(revID);
            Log("        ...rev %.*s", SPLAT(revID));

            // First row is the current revision:
            C4DocPutRequest put {};
            put.docID = newDoc->docID;
            put.existingRevision = true;
            put.revFlags =0;
            if (_currentRev->getColumn(3).getInt() != 0)
                put.revFlags = kRevDeleted;
            bool hasAttachments = _currentRev->getColumn(5).getInt() == 0;
            if (hasAttachments)
                put.revFlags |= kRevHasAttachments;

            // Convert the JSON body to Fleece:
            alloc_slice body;
            {
                Retained<Doc> doc = convertBody(asSlice(_currentRev->getColumn(4)));
                if (hasAttachments)
                    copyAttachments(doc);
                body = doc->allocedData();
            }
            put.allocedBody = {(void*)body.buf, body.size};

            int64_t nextSequence = _currentRev->getColumn(2);

            // Build the revision history:
            _parentRevs->reset();
            _parentRevs->bind(1, (long long)oldDocKey);
            while (_parentRevs->executeStep()) {
                if ((int64_t)_parentRevs->getColumn(0) == nextSequence) {
                    alloc_slice parentID(asSlice(_parentRevs->getColumn(1)));
                    history.push_back(parentID);
                    Log("        ...rev %.*s", SPLAT(parentID));
                    nextSequence = _parentRevs->getColumn(2);
                }
            }

            put.historyCount = history.size();
            put.history = (C4String*) history.data();
            put.save = true;
            C4Error error;
            if (!newDoc->putExistingRevision(put, &error))
                error::_throw((error::Domain)error.domain, error.code);
        }


        // Converts a JSON document body to Fleece.
        Retained<Doc> convertBody(slice json) {
            Encoder &enc = _newDB->sharedEncoder();
            JSONConverter converter(enc);
            if (!converter.encodeJSON(json))
                error::_throw(error::CorruptRevisionData, "invalid JSON data");
            return enc.finishDoc();
        }


        // Copies all blobs referenced in attachments of a revision from the old db.
        void copyAttachments(Doc *doc) {
            auto root = doc->asDict();
            if (!root) return;
            auto atts = root->get(C4STR(kC4LegacyAttachmentsProperty));
            if (!atts) return;
            auto attsDict = atts->asDict();
            if (!attsDict) return;
            for (Dict::iterator i(attsDict); i; ++i) {
                auto meta = i.value()->asDict();
                if (meta) {
                    auto digest = meta->get(slice(kC4BlobDigestProperty));
                    if (digest)
                        copyAttachment((string)digest->asString());
                }
            }
        }


        // Copies a blob to the new database if it exists in the old one.
        bool copyAttachment(string digest) {
            Log("        ...attachment '%s'", digest.c_str());
            blobKey key(digest);
            string hex = key.hexString();
            for (char &c : hex)
                c = (char)toupper(c);
            FilePath src = _attachments[hex + ".blob"];
            if (!src.exists())
                return false;

            //OPT: Could move the attachment file instead of copying (to save disk space)
            BlobWriteStream out(*_newDB->blobStore());
            char buf[32768];
            FileReadStream in(src);
            size_t bytesRead;
            while (0 != (bytesRead = in.read(buf, sizeof(buf))))
                out.write({buf, bytesRead});
            out.install(&key);
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

        FilePath _oldPath;
        SQLite::Database _oldDB;
        Retained<Database> _newDB;
        FilePath _attachments;
        unique_ptr<SQLite::Statement> _currentRev, _parentRevs;
    };


    void UpgradeDatabase(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig cfg) {
        Upgrader(oldPath, newPath, cfg).run();
    }


    bool UpgradeDatabaseInPlace(const FilePath &path, C4DatabaseConfig config) {
        if (config.flags & (kC4DB_NoUpgrade | kC4DB_ReadOnly)) return false;

        string p = path.path();
        chomp(p, '/');
        chomp(p, '\\');
        FilePath newTempPath(p + "_TEMP/");

        try {
            // Upgrade to a new db:
            auto newConfig = config;
            newConfig.flags |= kC4DB_Create;
            Log("Upgrader upgrading db <%s>; creating new db at <%s>",
                path.path().c_str(), newTempPath.path().c_str());
            UpgradeDatabase(path, newTempPath, newConfig);

            // Move the new db to the real path:
            newTempPath.moveToReplacingDir(path, true);
        } catch (...) {
            newTempPath.delRecursive();
            throw;
        }
        
        Log("Upgrader finished");
        return true;
    }

}
