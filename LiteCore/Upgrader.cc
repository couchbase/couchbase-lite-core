//
//  Upgrader.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4Internal.hh"
#include "Upgrader.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Database.hh"
#include "Document.hh"
#include "BlobStore.hh"
#include "Fleece.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <sqlite3.h>

using namespace std;
using namespace fleece;
using namespace c4Internal;

// If 1, `_attachment` dicts in revisions will be modified to add "_cbltype":"blob" and remove
// obsolete keys "stub", "follows", "revpos". But this is probably a bad idea because a revision
// is supposed to be globally immutable. --jpa 4/10/17
#define MODIFY_REVS 0

namespace litecore {

    class Upgrader {
    public:
        Upgrader(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig config)
        :Upgrader(oldPath, new Database(newPath.path(), config))
        { }


        Upgrader(const FilePath &oldPath, Database *newDB)
        :_oldDB(oldPath["db.sqlite3"].path(), SQLite::OPEN_READWRITE) // *
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
            _allRevs.reset(new SQLite::Statement(_oldDB,
                         "SELECT sequence, revid, parent, current, deleted, json, no_attachments"
                         " FROM revs WHERE doc_id=? ORDER BY sequence"));
        }


        // Top-level method to invoke the upgrader.
        void run() {
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
                        throw "missing parent sequence";//FIX
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
                    if (hasAttachments) {
#if MODIFY_REVS
                        body = convertAttachments(body);
#else
                        copyAttachments(body);
#endif
                    }
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
                throw "bad JSON"; //FIX
            return enc.extractOutput();
        }


        void copyAttachments(slice fleeceBody) {
            auto root = Value::fromTrustedData(fleeceBody)->asDict();
            if (!root) return;
            auto atts = root->get("_attachments"_sl, _sharedKeys);
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


        // Further converts a Fleece document body by removing "stub", "follows", "revpos" keys
        // from attachment metadata. Also copies all referenced blobs to the new db.
        // (Only called when MODIFY_REVS is set.)
        alloc_slice convertAttachments(slice fleeceBody) {
            Encoder &enc = _newDB->sharedEncoder();
            const Dict *root = Value::fromTrustedData(fleeceBody)->asDict();
            // Write the document top-level keys:
            enc.beginDictionary();
            for (Dict::iterator i(root, _sharedKeys); i; ++i) {
                slice key = i.keyString();
                enc.writeKey(key);
                if (key == "_attachments"_sl && i.value()->asDict()) {
                    // Write the _attachments dictionary:
                    enc.beginDictionary();
                    for (Dict::iterator att(i.value()->asDict(), _sharedKeys); att; ++att) {
                        enc.writeKey(att.keyString());
                        auto meta = att.value()->asDict();
                        if (meta) {
                            // Write an attachment:
                            writeAttachment(enc, meta);
                        } else {
                            enc.writeValue(att.value());
                        }
                    }
                    enc.endDictionary();
                } else {
                    enc.writeValue(i.value());
                }
            }
            enc.endDictionary();
            return enc.extractOutput();
        }


        // Copies attachment metadata to the encoder, omitting obsolete keys.
        // Also copies the referenced blob to the new db if it exists in the old one.
        // (Only called when MODIFY_REVS is set.)
        void writeAttachment(Encoder &enc, const Dict *attachment) {
            enc.beginDictionary();
            enc.writeKey("_cbltype"_sl);
            enc.writeString("blob"_sl);
            for (Dict::iterator meta(attachment, _sharedKeys); meta; ++meta) {
                slice key = meta.keyString();
                if (key != "stub"_sl && key != "follows"_sl && key != "revpos"_sl) {
                    if (key == "digest"_sl)
                        copyAttachment((string)meta.value()->asString());
                    enc.writeKey(key);
                    enc.writeValue(meta.value());
                }
            }
            enc.endDictionary();
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
                    throw "bad JSON"; //FIX
                }
                auto body = enc.extractOutput();
                _newDB->putRawDocument("_local", docID, revID, body);
            }
        }

    private:
        SQLite::Database _oldDB;
        Retained<Database> _newDB;
        SharedKeys* _sharedKeys;
        FilePath _attachments;
        unique_ptr<SQLite::Statement> _allRevs;
    };


    void UpgradeDatabase(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig cfg) {
        Upgrader(oldPath, newPath, cfg).run();
    }


    void UpgradeDatabase(const FilePath &oldPath, Database *newDB) {
        Upgrader(oldPath, newDB).run();
    }

}
