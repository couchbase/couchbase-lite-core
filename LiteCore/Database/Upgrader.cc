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
#include "Fleece.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <sqlite3.h>
#include <thread>

using namespace std;
using namespace fleece;
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
        ,_sharedKeys(newDB->documentKeys())
        ,_attachments(oldPath["attachments/"])
        {
            // * Note: It would be preferable to open the old db read-only, but that will fail
            // unless its '-shm' file already exists. <https://www.sqlite.org/wal.html#readonly>

            // This collation will never be called, but SQLite barfs unless it's registered:
            sqlite3_create_collation(_oldDB.getHandle(), "REVID", SQLITE_UTF8, NULL,
                                     [](void *context,
                                        int len1, const void * chars1,
                                        int len2, const void * chars2) -> int
                                     {
                                         abort();
                                     });
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

            _allRevs.reset(new SQLite::Statement(_oldDB,
                         "SELECT sequence, revid, parent, current, deleted, json, no_attachments"
                         " FROM revs WHERE doc_id=? ORDER BY sequence"));

            _newDB->beginTransaction();
            try {
                copyDocs();
                copyLocalDocs();
            } catch (...) {
                _newDB->endTransaction(false);
                throw;
            }
            _newDB->endTransaction(true);
        }


    private:

        static inline slice asSlice(const SQLite::Column &col) {
            return slice(col.getBlob(), col.size());
        }


        // Copies all documents to the new db.
        void copyDocs() {
            SQLite::Statement allDocs(_oldDB, "SELECT doc_id, docid FROM docs");
            while (allDocs.executeStep()) {
                int64_t docKey = allDocs.getColumn(0);
                slice docID = asSlice(allDocs.getColumn(1));

                Log("Importing doc '%.*s'", SPLAT(docID));
                unique_ptr<Document> newDoc(
                                _newDB->documentFactory().newDocumentInstance(toc4slice(docID)));
                copyRevisions(docKey, newDoc.get());
            }
        }


        // Copies all revisions of a document.
        void copyRevisions(int64_t oldDocKey, Document *newDoc) {
            map<int64_t, string> parentSequences;

            C4DocPutRequest put {};
            put.docID = newDoc->docID;
            put.existingRevision = put.allowConflict = true;
            put.maxRevTreeDepth = _newDB->maxRevTreeDepth();
            C4String history[2];
            put.history = history;

            _allRevs->reset();
            _allRevs->bind(1, (long long)oldDocKey);
            while (_allRevs->executeStep()) {
                // Get the revID and the parent's revID:
                string revID = _allRevs->getColumn(1);
                Log("        rev '%s'", revID.c_str());
                history[0] = revID;
                put.historyCount = 1;
                int64_t parentSequence = _allRevs->getColumn(2);
                if (parentSequence) {
                    auto i = parentSequences.find(parentSequence);
                    if (i == parentSequences.end())
                        error::_throw(error::CorruptData,
                                      "missing parent sequence while upgrading database");
                    history[1] = i->second;
                    ++put.historyCount;
                }

                // Remember the revID unless this is a leaf:
                bool current = _allRevs->getColumn(3).getInt() != 0;
                if (!current) {
                    int64_t sequence = _allRevs->getColumn(0);
                    parentSequences.emplace(sequence, revID);
                }

                // Set the revision flags:
                put.revFlags =0;
                if (_allRevs->getColumn(4).getInt() != 0)
                    put.revFlags = kRevDeleted;
                bool hasAttachments = _allRevs->getColumn(6).getInt() == 0;
                if (hasAttachments)
                    put.revFlags |= kRevHasAttachments;

                alloc_slice body;
                if (current) {
                    // Convert the JSON body to Fleece:
                    body = convertBody(asSlice(_allRevs->getColumn(5)));
                    if (hasAttachments)
                        copyAttachments(body);
                    put.body = body;
                } else {
                    put.body = nullslice;
                }

                // Now add the revision:
                newDoc->putExistingRevision(put);
            }

            newDoc->save();
        }


        // Converts a JSON document body to Fleece.
        alloc_slice convertBody(slice json) {
            Encoder &enc = _newDB->sharedEncoder();
            JSONConverter converter(enc);
            if (!converter.encodeJSON(json))
                error::_throw(error::CorruptData, "invalid JSON data in database being upgraded");
            return enc.extractOutput();
        }


        void copyAttachments(slice fleeceBody) {
            auto root = Value::fromTrustedData(fleeceBody)->asDict();
            if (!root) return;
            auto atts = root->get(C4STR(kC4LegacyAttachmentsProperty), _sharedKeys);
            if (!atts) return;
            auto attsDict = atts->asDict();
            if (!attsDict) return;
            for (Dict::iterator i(attsDict, _sharedKeys); i; ++i) {
                auto meta = i.value()->asDict();
                if (meta) {
                    auto digest = meta->get("digest"_sl, _sharedKeys);
                    if (digest)
                        copyAttachment((string)digest->asString());
                }
            }
        }


        // Copies a blob to the new database if it exists in the old one.
        bool copyAttachment(string digest) {
            Log("            attachment '%s'", digest.c_str());
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
                auto body = enc.extractOutput();
                _newDB->putRawDocument("_local", docID, revID, body);
            }
        }

        FilePath _oldPath;
        SQLite::Database _oldDB;
        Retained<Database> _newDB;
        SharedKeys* _sharedKeys;
        FilePath _attachments;
        unique_ptr<SQLite::Statement> _allRevs;
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
