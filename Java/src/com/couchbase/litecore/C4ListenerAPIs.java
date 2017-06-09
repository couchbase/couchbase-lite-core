package com.couchbase.litecore;

/**
 * Flags indicating which network API(s) to serve.
 */
public interface C4ListenerAPIs {
    int kC4RESTAPI = 0x01; ///< CouchDB-like REST API
    int kC4SyncAPI = 0x02; ///< Replication server
}
