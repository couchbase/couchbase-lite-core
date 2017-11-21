package com.couchbase.litecore.fleece;

public class FleeceDocument {
    private MRoot _root = null;

    FleeceDocument(AllocSlice fleeceData, FLSharedKeys sharedKeys, boolean mutableContainers) {
        _root = new MRoot(fleeceData, sharedKeys, mutableContainers);
    }

    public static Object getObject(AllocSlice fleeceData, FLSharedKeys sharedKeys, boolean mutableContainers) {
        MRoot root = new MRoot(fleeceData, sharedKeys, mutableContainers);
        return root.asNative();
    }

    public Object rootObject() {
        return _root.asNative();
    }

    @Override
    protected void finalize() throws Throwable {
        if (_root != null) {
            _root.free();
            _root = null;
        }
        super.finalize();
    }
}
