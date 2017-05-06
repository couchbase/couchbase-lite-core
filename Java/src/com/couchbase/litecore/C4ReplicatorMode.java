package com.couchbase.litecore;

// How to replicate, in either direction
public interface C4ReplicatorMode {
    int kC4Disabled = 0;   // Do not allow this direction
    int kC4Passive = 1;    // Allow peer to initiate this direction
    int kC4OneShot = 2;    // Replicate, then stop
    int kC4Continuous = 3; // Keep replication active until stopped by application
}
