package com.couchbase.litecore.fleece;

import com.couchbase.litecore.SharedKeys;

import java.util.concurrent.atomic.AtomicBoolean;

public class MValueDelegate implements MValue.Delegate, FLConstants.FLValueType {
    @Override
    public Object toNative(MValue mv, MCollection parent, AtomicBoolean cacheIt) {
        FLValue value = mv.getValue();
        int type = value.getType();
        switch (type) {
            case kFLArray:
                cacheIt.set(true);
                return new FleeceArray(mv, parent);
            case kFLDict:
                cacheIt.set(true);
                return new FleeceDict(mv, parent);
            default:
                return value.toObject(new SharedKeys(parent.getContext().getSharedKeys()));
        }
    }

    @Override
    public MCollection collectionFromNative(Object object) {
        if (object instanceof FleeceArray)
            return ((FleeceArray)object).toMCollection();
        else if (object instanceof FleeceDict)
            return ((FleeceDict)object).toMCollection();
        else
            return null;
    }

    @Override
    public void encodeNative(Encoder enc, Object object) {
        if (object == null)
            enc.writeNull();
        else
            enc.writeObject(object);
    }
}
