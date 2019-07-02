{
  global:
    c4_BOGUS;
    c4_getVersion;
    c4_now;

    c4log_getLevel;
    c4log_setLevel;
    c4slog;
    c4log_writeToCallback;
    c4log_writeToBinaryFile;
    c4log_callbackLevel;
    c4log_setCallbackLevel;
    c4log_binaryFileLevel;
    c4log_setBinaryFileLevel;
    c4log_getDomain;
    c4log_getDomainName;
    c4log_warnOnErrors;
    c4log_willLog;

    c4error_make;
    c4error_getMessage;
    c4error_mayBeTransient;
    c4error_mayBeNetworkDependent;

    c4key_setPassword;

    c4db_open;
    c4db_openNamed;
    c4db_retain;
    c4db_free;
    c4db_close;
    c4db_copy;
    c4db_copyNamed;
    c4db_delete;
    c4db_deleteAtPath;
    c4db_deleteNamed;
    c4db_exists;
    c4db_compact;
    c4db_rekey;
    c4db_getPath;
    c4db_getConfig;
    c4db_getDocumentCount;
    c4db_getLastSequence;
    c4db_getMaxRevTreeDepth;
    c4db_setMaxRevTreeDepth;
    c4db_getUUIDs;
    c4db_getExtraInfo;
    c4db_setExtraInfo;
    c4db_beginTransaction;
    c4db_endTransaction;
    c4db_isInTransaction;
    c4db_getSharedFleeceEncoder;
    c4db_getFLSharedKeys;
    c4db_encodeJSON;

    c4raw_free;
    c4raw_get;
    c4raw_put;

    c4doc_generateID;
    c4doc_retain;
    c4doc_free;
    c4doc_get;
    c4doc_getBySequence;
    c4doc_getSingleRevision;
    c4db_purgeDoc;
    c4doc_selectRevision;
    c4doc_selectCurrentRevision;
    c4doc_loadRevisionBody;
    c4doc_hasRevisionBody;
    c4doc_selectParentRevision;
    c4doc_selectNextRevision;
    c4doc_selectNextLeafRevision;
    c4doc_selectCommonAncestorRevision;
    c4doc_selectFirstPossibleAncestorOf;
    c4doc_selectNextPossibleAncestorOf;
    c4doc_put;
    c4doc_create;
    c4doc_update;
    c4doc_resolveConflict;
    c4doc_purgeRevision;
    c4doc_save;
    c4doc_setExpiration;
    c4doc_getExpiration;
    c4db_nextDocExpiration;
    c4db_purgeExpiredDocs;
    c4doc_containingValue;
    c4doc_isOldMetaProperty;
    c4doc_dictContainsBlobs;

    c4rev_getGeneration;

    c4db_enumerateChanges;
    c4db_enumerateAllDocs;
    c4db_createIndex;
    c4db_deleteIndex;
    c4db_getIndexes;
    c4enum_next;
    c4enum_getDocumentInfo;
    c4enum_getDocument;
    c4enum_close;
    c4enum_free;

    c4queryenum_next;
    c4queryenum_seek;
    c4queryenum_refresh;
    c4queryenum_close;
    c4queryenum_retain;
    c4queryenum_free;

    c4query_new;
    c4query_new2;
    c4query_retain;
    c4query_free;
    c4query_setParameters;
    c4query_columnCount;
    c4query_columnTitle;
    c4query_run;
    c4query_explain;

    c4blob_keyFromString;
    c4blob_keyToString;
    c4blob_openStore;
    c4blob_deleteStore;
    c4blob_getSize;
    c4blob_getContents;
    c4blob_getFilePath;
    c4blob_openReadStream;
    c4blob_create;
    c4blob_delete;
    c4blob_openWriteStream;
    c4db_getBlobStore;

    c4stream_read;
    c4stream_getLength;
    c4stream_seek;
    c4stream_close;

    c4stream_write;
    c4stream_bytesWritten;
    c4stream_computeBlobKey;
    c4stream_install;
    c4stream_closeWriter;

    c4dbobs_create;
    c4dbobs_getChanges;
    c4dbobs_releaseChanges;
    c4dbobs_free;
    c4docobs_create;
    c4docobs_free;
    c4queryobs_create;
    c4queryobs_free;
    c4queryobs_getEnumerator;

    c4repl_new;
    c4repl_newWithSocket;
    c4repl_free;
    c4repl_stop;
    c4repl_getStatus;

    c4socket_registerFactory;
    c4socket_fromNative;
    c4socket_opened;
    c4socket_closeRequested;
    c4socket_closed;
    c4socket_completedWrite;
    c4socket_received;
    c4socket_gotHTTPResponse;

    c4pred_registerModel;
    c4pred_unregisterModel;

    c4_getObjectCount;
    c4_shutdown;

    FLSlice_Equal;
    FLSlice_Compare;
    FLSliceResult_Release;
    FLSlice_Copy;

    FLDoc_FromResultData;
    FLDoc_GetRoot;
    FLDoc_GetSharedKeys;
    FLDoc_Release;

    FLValue_FromData;
    FLValue_FindDoc;
    FLValue_GetType;
    FLValue_IsInteger;
    FLValue_IsUnsigned;
    FLValue_IsDouble;
    FLValue_AsBool;
    FLValue_AsData;
    FLValue_AsInt;
    FLValue_AsUnsigned;
    FLValue_AsFloat;
    FLValue_AsDouble;
    FLValue_AsString;
    FLValue_AsArray;
    FLValue_AsDict;
    FLValue_ToString;
    FLValue_ToJSON;
    FLValue_ToJSONX;

    FLData_ConvertJSON;
    FLJSON5_ToJSON;

    FLArray_Count;
    FLArray_Get;
    FLArrayIterator_Begin;
    FLArrayIterator_GetCount;
    FLArrayIterator_GetValue;
    FLArrayIterator_GetValueAt;
    FLArrayIterator_Next;

    FLDict_Count;
    FLDict_Get;
    FLDict_GetWithKey;
    FLDictIterator_Begin;
    FLDictIterator_GetKey;
    FLDictIterator_GetKeyString;
    FLDictIterator_GetValue;
    FLDictIterator_Next;
    FLDictKey_Init;
    FLDictKey_GetString;

    FLEncoder_GetExtraInfo;
    FLEncoder_SetExtraInfo;
    FLEncoder_New;
    FLEncoder_Free;
    FLEncoder_Reset;
    FLEncoder_WriteNull;
    FLEncoder_WriteBool;
    FLEncoder_WriteInt;
    FLEncoder_WriteUInt;
    FLEncoder_WriteFloat;
    FLEncoder_WriteDouble;
    FLEncoder_WriteString;
    FLEncoder_WriteData;
    FLEncoder_WriteValue;
    FLEncoder_BeginArray;
    FLEncoder_EndArray;
    FLEncoder_BeginDict;
    FLEncoder_WriteKey;
    FLEncoder_WriteKeyValue;
    FLEncoder_EndDict;
    FLEncoder_FinishDoc;
    FLEncoder_Finish;

    kC4SQLiteStorageEngine;
    kC4DatabaseFilenameExtension;

    c4_getBuildInfo;

    c4log;
    c4vlog;
    kC4Cpp_DefaultLog;
    kC4DefaultLog;
    kC4DatabaseLog;
    kC4QueryLog;
    kC4SyncLog;
    kC4WebSocketLog;

    c4error_getDescription;
    c4error_getDescriptionC;

    c4db_openAgain;
    c4db_createFleeceEncoder;
    c4db_lock;
    c4db_unlock;
    c4db_getRemoteDBID;

    c4doc_removeRevisionBody;
    c4doc_getForPut;
    c4doc_bodyAsJSON;
    c4doc_hasOldMetaProperties;
    c4doc_encodeStrippingOldMetaProperties;
    c4doc_detachRevisionBody;
    c4doc_getRemoteAncestor;
    c4doc_getBlobData;

    c4db_getIndexesInfo;

    kC4DefaultEnumeratorOptions;
    kC4DefaultQueryOptions;

    c4queryenum_getRowCount;

    c4query_fullTextMatched;

    c4blob_computeKey;

    c4repl_isValidDatabaseName;
    c4repl_isValidRemote;
    c4repl_getResponseHeaders;

    kC4ReplicatorActivityLevelNames;
    kC4SocketOptionWSProtocols;

    c4address_fromURL;
    c4address_toURL;

    c4error_return;
    c4db_markSynced;
    c4_dumpInstances;
    gC4ExpectExceptions;

    FLSliceResult_Retain;

    FLDoc_FromJSON;
    FLDoc_Retain;

    FLEncoder_NewWithOptions;
    FLEncoder_WriteRaw;
    FLEncoder_ConvertJSON;
    FLEncoder_GetError;

    FLSharedKeys_Decode;
    FLSharedKeys_Retain;
    FLSharedKeys_Release;

    FLValue_ToJSON5;

    FLEncoder_WriteNSObject;
    
  local:
    *;
};
