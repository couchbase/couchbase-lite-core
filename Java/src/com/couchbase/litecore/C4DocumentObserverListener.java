package com.couchbase.litecore;

public interface C4DocumentObserverListener {
    void callback(C4DocumentObserver observer, String docID, long sequence, Object context);
}
