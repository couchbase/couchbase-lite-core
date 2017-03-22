package com.couchbase.litecore;

public class StopWatch {
    private long startTime = 0; // nano seconds
    private long stopTime = 0;
    private boolean running = false;

    public StopWatch() {
        start();
    }

    public void start() {
        this.startTime = System.nanoTime();
        this.running = true;
    }

    public void stop() {
        this.stopTime = System.nanoTime();
        this.running = false;
    }

    public long getElapsedTime() {
        long elapsed;
        if (running) {
            elapsed = (System.nanoTime() - startTime);
        } else {
            elapsed = (stopTime - startTime);
        }
        return elapsed;
    }

    public double getElapsedTimeMillis() {
        double elapsed;
        if (running) {
            elapsed = ((double) (System.nanoTime() - startTime) / 1000000.0);
        } else {
            elapsed = ((double) (stopTime - startTime) / 1000000.0);
        }
        return elapsed;
    }

    public double getElapsedTimeSecs() {
        double elapsed;
        if (running) {
            elapsed = ((double) (System.nanoTime() - startTime) / 1000000000.0);
        } else {
            elapsed = ((double) (stopTime - startTime) / 1000000000.0);
        }
        return elapsed;
    }

    public String toString(String what, long count, String item) {
        return String.format("%s; %d %s (took %.3f ms)", what, count, item, getElapsedTimeMillis());
    }
}
