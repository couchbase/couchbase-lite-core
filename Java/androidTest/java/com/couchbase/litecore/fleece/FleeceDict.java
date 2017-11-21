package com.couchbase.litecore.fleece;

import java.util.Collection;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

public class FleeceDict implements Map<String, Object>, Encodable, FLCollection {
    private MDict _dict;

    private FleeceDict() {
        _dict = new MDict();
    }

    // Call from native method
    FleeceDict(long mv, long parent) {
        this();
        _dict.initInSlot(mv, parent);
    }

    public boolean isMutated() {
        return _dict.isMutated();
    }

    @Override
    public int size() {
        return (int) _dict.count();
    }

    @Override
    public boolean isEmpty() {
        return size() == 0;
    }

    @Override
    public boolean containsKey(Object o) {
        if (!(o instanceof String)) return false;
        return _dict.contains((String) o);
    }

    @Override
    public boolean containsValue(Object o) {
        throw new UnsupportedOperationException();
    }

    @Override
    public Object get(Object key) {
        if (!(key instanceof String)) return null;
        return _dict.get((String) key).asNative(_dict);
    }

    @Override
    public Object put(String key, Object o) {
        Object prev = null;
        if (_dict.contains(key))
            prev = _dict.get(key);
        if (!_dict.set(key, o))
            throw new IllegalStateException();
        return prev;
    }

    @Override
    public Object remove(Object key) {
        if (!(key instanceof String)) return null;
        Object prev = null;
        if (_dict.contains((String) key))
            prev = get(key);
        if (!_dict.remove((String) key))
            throw new IllegalStateException();
        return prev;
    }

    @Override
    public void putAll(Map<? extends String, ?> map) {
        throw new UnsupportedOperationException();
    }

    @Override
    public void clear() {
        if (!_dict.clear())
            throw new IllegalStateException();
    }

    @Override
    public Set<String> keySet() {
        Set<String> keys = new HashSet<>();
        MDictIterator itr = new MDictIterator(_dict);
        String key;
        while ((key = itr.key()) != null) {
            keys.add(key);
            if (!itr.next())
                break;
        }
        return keys;
    }

    @Override
    public Collection<Object> values() {
        throw new UnsupportedOperationException();
    }

    @Override
    public Set<Entry<String, Object>> entrySet() {
        throw new UnsupportedOperationException();
    }

    // Implementation of FLEncodable
    @Override
    public void encodeTo(Encoder enc) {
        _dict.encodeTo(enc);
    }

    // Implementation of FLCollection
    @Override
    public long getFLCollectionHandle() {
        return _dict._handle;
    }
}
