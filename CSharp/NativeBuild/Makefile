FORESTDB_PATH   =  ../../vendor/forestdb
SNAPPY_PATH =  ../../vendor/snappy
SQLITE3_PATH=  ../../vendor/sqlite3-unicodesn
CBFOREST_PATH   =  ../../CBForest

CC := clang++

CFLAGS := $(USR_CFLAGS) \
-c \
-I$(SQLITE3_PATH)/libstemmer_c/runtime/ \
-I$(SQLITE3_PATH)/libstemmer_c/src_c/ \
-I$(SQLITE3_PATH)/ \
-DSQLITE_ENABLE_FTS4 \
-DSQLITE_ENABLE_FTS4_UNICODE61 \
-DWITH_STEMMER_english \
-DDOC_COMP \
-D_DOC_COMP \
-DHAVE_GCC_ATOMICS=1 \
-fPIC 

CPPFLAGS += $(USR_CPPFLAGS) \
-c \
-I$(SQLITE3_PATH)/libstemmer_c/runtime/ \
-I$(SQLITE3_PATH)/libstemmer_c/src_c/ \
-I$(SQLITE3_PATH) \
-I$(FORESTDB_PATH)/include/ \
-I$(FORESTDB_PATH)/include/libforestdb/ \
-I$(FORESTDB_PATH)/src/ \
-I$(FORESTDB_PATH)/utils/ \
-I$(FORESTDB_PATH)/option/ \
-I$(SNAPPY_PATH)/ \
-I$(CBFOREST_PATH)/ \
-I$(CBFOREST_PATH)/Encryption \
-DFORESTDB_VERSION=\"Internal\" \
-DSQLITE_ENABLE_FTS4 \
-DSQLITE_ENABLE_FTS4_UNICODE61 \
-DWITH_STEMMER_english \
-DDOC_COMP \
-D_DOC_COMP \
-DHAVE_GCC_ATOMICS=1 \
-D_CRYPTO_OPENSSL \
-std=c++11 \
-fexceptions \
-fpermissive \
-frtti \
-Wno-unused-value \
-Wno-deprecated-register \
-include $(CBFOREST_PATH)/CBForest-Prefix.pch \
-fPIC 

LDFLAGS += $(USR_LDFLAGS) -lpthread -lcrypto -shared -Wl

SOURCES = $(SQLITE3_PATH)/fts3_unicode2.o \
$(SQLITE3_PATH)/fts3_unicodesn.o \
$(SQLITE3_PATH)/libstemmer_c/runtime/api_sq3.o \
$(SQLITE3_PATH)/libstemmer_c/runtime/utilities_sq3.o \
$(SQLITE3_PATH)/libstemmer_c/libstemmer/libstemmer_utf8.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_danish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_dutch.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_english.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_finnish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_french.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_german.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_hungarian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_italian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_norwegian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_porter.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_portuguese.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_spanish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_1_swedish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_ISO_8859_2_romanian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_KOI8_R_russian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_danish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_dutch.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_english.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_finnish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_french.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_german.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_hungarian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_italian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_norwegian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_porter.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_portuguese.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_romanian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_russian.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_spanish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_swedish.o \
$(SQLITE3_PATH)/libstemmer_c/src_c/stem_UTF_8_turkish.o \
$(FORESTDB_PATH)/utils/crc32.o \
$(FORESTDB_PATH)/utils/debug.o \
$(FORESTDB_PATH)/utils/iniparser.o \
$(FORESTDB_PATH)/utils/memleak.o \
$(FORESTDB_PATH)/utils/partiallock.o \
$(FORESTDB_PATH)/utils/system_resource_stats.o \
$(FORESTDB_PATH)/utils/time_utils.o \
$(FORESTDB_PATH)/src/api_wrapper.o \
$(FORESTDB_PATH)/src/avltree.o \
$(FORESTDB_PATH)/src/bgflusher.o \
$(FORESTDB_PATH)/src/blockcache.o \
$(FORESTDB_PATH)/src/btree.o \
$(FORESTDB_PATH)/src/btree_fast_str_kv.o \
$(FORESTDB_PATH)/src/btree_kv.o \
$(FORESTDB_PATH)/src/btree_str_kv.o \
$(FORESTDB_PATH)/src/btreeblock.o \
$(FORESTDB_PATH)/src/checksum.o \
$(FORESTDB_PATH)/src/staleblock.o \
$(FORESTDB_PATH)/src/compactor.o \
$(FORESTDB_PATH)/src/configuration.o \
$(FORESTDB_PATH)/src/docio.o \
$(FORESTDB_PATH)/src/encryption.o \
$(FORESTDB_PATH)/src/encryption_aes.o \
$(FORESTDB_PATH)/src/encryption_bogus.o \
$(FORESTDB_PATH)/src/fdb_errors.o \
$(FORESTDB_PATH)/src/filemgr.o \
$(FORESTDB_PATH)/src/filemgr_ops.o \
$(FORESTDB_PATH)/src/filemgr_ops_linux.o \
$(FORESTDB_PATH)/src/forestdb.o \
$(FORESTDB_PATH)/src/hash.o \
$(FORESTDB_PATH)/src/hash_functions.o \
$(FORESTDB_PATH)/src/hbtrie.o \
$(FORESTDB_PATH)/src/iterator.o \
$(FORESTDB_PATH)/src/kv_instance.o \
$(FORESTDB_PATH)/src/list.o \
$(FORESTDB_PATH)/src/snapshot.o \
$(FORESTDB_PATH)/src/transaction.o \
$(FORESTDB_PATH)/src/wal.o \
$(FORESTDB_PATH)/src/version.o \
$(SNAPPY_PATH)/snappy.o \
$(SNAPPY_PATH)/snappy-c.o \
$(SNAPPY_PATH)/snappy-sinksource.o \
$(SNAPPY_PATH)/snappy-stubs-internal.o \
$(CBFOREST_PATH)/slice.o \
$(CBFOREST_PATH)/varint.o \
$(CBFOREST_PATH)/Collatable.o \
$(CBFOREST_PATH)/Database.o \
$(CBFOREST_PATH)/DocEnumerator.o \
$(CBFOREST_PATH)/Document.o \
$(CBFOREST_PATH)/Error.o \
$(CBFOREST_PATH)/Geohash.o \
$(CBFOREST_PATH)/GeoIndex.o \
$(CBFOREST_PATH)/Index.o \
$(CBFOREST_PATH)/KeyStore.o \
$(CBFOREST_PATH)/RevID.o \
$(CBFOREST_PATH)/RevTree.o \
$(CBFOREST_PATH)/VersionedDocument.o \
$(CBFOREST_PATH)/MapReduceIndex.o \
$(CBFOREST_PATH)/Tokenizer.o \
../../C/c4.o \
../../C/c4Database.o \
../../C/c4View.o \
../../C/c4Key.o \
../../C/c4Document.o \
../../C/c4DocEnumerator.o

TARGET=libCBForest-Interop.so

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(LDFLAGS) -o $@ $^
	bash gen_linux_symbol_list.sh
	strip @stripopts

%.o: %.c 
	$(CC) $(CFLAGS) -o $@ -x c $<

%.o: %.cc
	$(CC) $(CPPFLAGS) -o $@ -x c++ $<

clean:
	rm -f $(TARGET) `find ../.. -name *.o`

install:
	install -vD -m755 $(TARGET) ../prebuilt/$(TARGET)
