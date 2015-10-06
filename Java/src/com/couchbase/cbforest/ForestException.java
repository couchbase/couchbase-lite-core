package com.couchbase.cbforest;

public class ForestException extends Exception {
    public int domain; // TODO: Should be an enum
    public int code;

    public static void throwException(int domain, int code) throws ForestException {
        ForestException x = new ForestException();
        x.domain = domain;
        x.code = code;
        throw x;
    }

    @Override
    public String toString() {
        return "ForestException{" +
                "domain=" + domain +
                ", code=" + code +
                '}';
    }
}
