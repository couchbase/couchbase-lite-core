package com.couchbase.litecore;

public class C4QueryOptions {
    long skip = 0;
    long limit = Long.MAX_VALUE;
    boolean rankFullText = true;

    public C4QueryOptions() {
    }

    public C4QueryOptions(long skip, long limit, boolean rankFullText) {
        this.skip = skip;
        this.limit = limit;
        this.rankFullText = rankFullText;
    }

    public long getSkip() {
        return skip;
    }

    public void setSkip(long skip) {
        this.skip = skip;
    }

    public long getLimit() {
        return limit;
    }

    public void setLimit(long limit) {
        this.limit = limit;
    }

    public boolean isRankFullText() {
        return rankFullText;
    }

    public void setRankFullText(boolean rankFullText) {
        this.rankFullText = rankFullText;
    }
}
