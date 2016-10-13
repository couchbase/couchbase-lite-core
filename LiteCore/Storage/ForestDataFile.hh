//
//  ForestDataFile.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/25/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "DataFile.hh"
#include "fdb_types.h"

namespace litecore {

    class ForestKeyStore;


    /** ForestDB implementation of DataFile */
    class ForestDataFile : public DataFile {
    public:
        
        static fdb_config defaultConfig();
        static void setDefaultConfig(const fdb_config&);

        ForestDataFile(const FilePath &path, const Options* =nullptr);
        ForestDataFile(const FilePath &path, const Options*, const fdb_config&);
        ~ForestDataFile();

        fdb_file_info info() const;
        fdb_config config() const                   {return _config;}

        static void shutdown();

        // Inherited methods:
        bool isOpen() const noexcept override;
        void close() override;
        void deleteDataFile() override;
        void reopen() override;
        std::vector<std::string> allKeyStoreNames() override;
        void compact() override;
        bool setAutoCompact(bool autoCompact) override;
        void rekey(EncryptionAlgorithm, slice newKey) override;

        class Factory : public DataFile::Factory {
        public:
            virtual const char* cname() override                {return "ForestDB";}
            virtual std::string filenameExtension() override    {return ".forestdb";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual ForestDataFile* openFile(const FilePath &, const Options* =nullptr) override;
            virtual bool deleteFile(const FilePath &path, const Options* =nullptr) override;
            virtual bool fileExists(const FilePath &path) override;
        };

        static Factory& factory();

    protected:
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
        void deleteKeyStore(const std::string &name) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*, bool commit) override;

    private:
        friend class ForestKeyStore;
        friend class ForestDataFileStorage;

        static fdb_compact_decision compactionCallback(fdb_file_handle *fhandle,
                                                       fdb_compaction_status status,
                                                       const char *kv_store_name,
                                                       fdb_doc *doc,
                                                       uint64_t last_oldfile_offset,
                                                       uint64_t last_newfile_offset,
                                                       void *ctx);
        bool onCompact(fdb_compaction_status status,
                       const char *kv_store_name,
                       fdb_doc *doc,
                       uint64_t lastOldFileOffset,
                       uint64_t lastNewFileOffset);

        fdb_config          _config;                    // ForestDB database configuration
        fdb_file_handle*    _fileHandle {nullptr};      // ForestDB database handle
    };



    /** ForestDB implementation of KeyStore. */
    class ForestKeyStore : public KeyStore {
    public:
        uint64_t recordCount() const override;
        sequence lastSequence() const override;

        Record get(sequence, ContentOptions) const override;
        bool read(Record &rec, ContentOptions options) const override;
        void readBody(Record &rec) const override;
        Record getByOffsetNoErrors(uint64_t offset, sequence seq) const override;

        setResult set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

    protected:
        ForestKeyStore(ForestDataFile&, const std::string &name, KeyStore::Capabilities options);
        ~ForestKeyStore();

        void reopen() override;
        void close() override;

        bool _del(slice key, Transaction&) override;
        bool _del(sequence s, Transaction&) override;

        void setDocNoKey(Record &rec, fdb_doc &fdoc) const;
        void setDoc(Record &rec, fdb_doc &fdoc) const;

        RecordEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, RecordEnumerator::Options&) override;
        RecordEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, RecordEnumerator::Options&) override;

    private:
        friend class ForestDataFile;
        friend class ForestEnumerator;

        fdb_kvs_handle* _handle {nullptr};  // ForestDB key-value store handle
    };
}

