package com.couchbase.litecore;

public class C4DatabaseChange {
    private String docID = null;
    private String revID = null;
    private long sequence = 0L;
    private long bodySize = 0L;
    private boolean external = false;

    public C4DatabaseChange() {
    }

    public String getDocID() {
        return docID;
    }

    public String getRevID() {
        return revID;
    }

    public long getSequence() {
        return sequence;
    }

    public long getBodySize() {
        return bodySize;
    }

    public boolean isExternal() {
        return external;
    }

    @Override
    public String toString() {
        return "C4DatabaseChange{" +
                "docID='" + docID + '\'' +
                ", revID='" + revID + '\'' +
                ", sequence=" + sequence +
                ", bodySize=" + bodySize +
                ", external=" + external +
                '}';
    }
}
