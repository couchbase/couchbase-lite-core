package com.couchbase.litecore;

public class C4Error {
    private int domain = 0;        // C4Error.domain
    private int code = 0;          // C4Error.code
    private int internalInfo = 0;  // C4Error.internal_info

    public C4Error(int domain, int code, int internalInfo) {
        this.domain = domain;
        this.code = code;
        this.internalInfo = internalInfo;
    }

    public int getDomain() {
        return domain;
    }

    public int getCode() {
        return code;
    }

    public int getInternalInfo() {
        return internalInfo;
    }

    @Override
    public String toString() {
        return "C4Error{" +
                "domain=" + domain +
                ", code=" + code +
                ", internalInfo=" + internalInfo +
                '}';
    }
}
