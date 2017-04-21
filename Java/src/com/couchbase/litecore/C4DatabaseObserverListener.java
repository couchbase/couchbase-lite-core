package com.couchbase.litecore;

public interface C4DatabaseObserverListener {
    void callback(C4DatabaseObserver observer, Object context);
}
