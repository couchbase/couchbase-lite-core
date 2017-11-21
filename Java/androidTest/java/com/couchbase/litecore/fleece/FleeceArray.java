package com.couchbase.litecore.fleece;

import java.util.Collection;
import java.util.Iterator;
import java.util.List;
import java.util.ListIterator;
import java.util.NoSuchElementException;

public class FleeceArray implements List<Object>, Encodable, FLCollection {
    private MArray _array;

    private FleeceArray() {
        _array = new MArray();
    }

    // Call from native method
    FleeceArray(long mv, long parent) {
        this();
        _array.initInSlot(mv, parent);
    }

    public boolean isMutated() {
        return _array.isMutated();
    }

    @Override
    public int size() {
        return (int) _array.count();
    }

    @Override
    public boolean isEmpty() {
        return size() == 0;
    }

    @Override
    public boolean contains(Object o) {
        throw new UnsupportedOperationException();
    }

    @Override
    public Iterator<Object> iterator() {
        return new Itr();
    }

    @Override
    public Object[] toArray() {
        throw new UnsupportedOperationException();
    }

    @Override
    public <T> T[] toArray(T[] ts) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean add(Object o) {
        return _array.append(o);
    }

    @Override
    public boolean remove(Object o) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean containsAll(Collection<?> collection) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean addAll(Collection<?> collection) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean addAll(int i, Collection<?> collection) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean removeAll(Collection<?> collection) {
        throw new UnsupportedOperationException();
    }

    @Override
    public boolean retainAll(Collection<?> collection) {
        throw new UnsupportedOperationException();
    }

    @Override
    public void clear() {
        if (!_array.clear())
            throw new IllegalStateException();
    }

    @Override
    public Object get(int i) {
        MValue val = _array.get(i);
        if (val.isEmpty())
            throw new IndexOutOfBoundsException();
        return val.asNative(_array);
    }

    @Override
    public Object set(int i, Object o) {
        Object prev = get(i);
        if (!_array.set(i, o)) {
            requireMutable();
            throw new IndexOutOfBoundsException();
        }
        return prev;
    }

    @Override
    public void add(int i, Object o) {
        if (!_array.insert(i, o)) {
            requireMutable();
            throw new IndexOutOfBoundsException();
        }
    }

    @Override
    public Object remove(int i) {
        Object prev = get(i);
        if (!_array.remove(i)) {
            requireMutable();
            throw new IndexOutOfBoundsException();
        }
        return prev;
    }

    @Override
    public int indexOf(Object o) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int lastIndexOf(Object o) {
        throw new UnsupportedOperationException();
    }

    @Override
    public ListIterator<Object> listIterator() {
        return new ListItr(0);
    }

    @Override
    public ListIterator<Object> listIterator(int index) {
        if (index < 0 || index > size())
            throw new IndexOutOfBoundsException("Index: " + index);
        return new ListItr(index);
    }

    @Override
    public List<Object> subList(int i, int i1) {
        throw new UnsupportedOperationException();
    }

    private void requireMutable() {
        if (!_array.isMutable())
            throw new IllegalStateException();
    }

    // Implementation of FLEncodable
    @Override
    public void encodeTo(Encoder enc) {
        _array.encodeTo(enc);
    }

    // Implementation of FLCollection
    @Override
    public long getFLCollectionHandle() {
        return _array._handle;
    }

    private class Itr implements Iterator<Object> {
        int cursor = 0;       // index of next element to return

        @Override
        public boolean hasNext() {
            return cursor != size();
        }

        @Override
        public Object next() {
            int i = cursor;
            if (i >= size()) throw new NoSuchElementException();
            cursor = i + 1;
            return get(i);
        }

        @Override
        public void remove() {
            throw new UnsupportedOperationException();
        }
    }

    private class ListItr extends Itr implements ListIterator<Object> {
        ListItr(int index) {
            super();
            cursor = index;
        }

        public boolean hasPrevious() {
            return cursor != 0;
        }

        public int nextIndex() {
            return cursor;
        }

        public int previousIndex() {
            return cursor - 1;
        }

        public Object previous() {
            int i = cursor - 1;
            if (i < 0)
                throw new NoSuchElementException();
            cursor = i;
            return get(i);
        }

        public void set(Object e) {
            throw new UnsupportedOperationException();
        }

        public void add(Object e) {
            throw new UnsupportedOperationException();
        }
    }
}
