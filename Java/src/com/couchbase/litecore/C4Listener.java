package com.couchbase.litecore;

public class C4Listener {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Listener

    //-------------------------------------------------------------------------
    // public static methods
    //-------------------------------------------------------------------------

    public static int getAvailableAPIs() {
        return availableAPIs();
    }

    public static String getURINameFromPath(String path) {
        return uriNameFromPath(path);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public C4Listener(C4ListenerConfig config) throws LiteCoreException {
        if (config == null)
            throw new IllegalArgumentException();

        handle = start(
                config.getPort(),
                config.getApis(),
                config.getDirectory(),
                config.isAllowCreateDBs(),
                config.isAllowDeleteDBs(),
                config.isAllowPush(),
                config.isAllowPull());
    }

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    public boolean shareDB(String name, Database db) {
        return shareDB(handle, name, db._handle);
    }

    public boolean unshareDB(String name) {
        return unshareDB(handle, name);
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------
    private static native int availableAPIs();

    private static native long start(int port,
                                     int apis,
                                     String directory,
                                     boolean allowCreateDBs,
                                     boolean allowDeleteDBs,
                                     boolean allowPush, boolean allowPull) throws LiteCoreException;

    private static native void free(long listener);

    /**
     * Makes a database available from the network, under the given name.
     */
    private static native boolean shareDB(long listener, String name, long db);

    /**
     * Makes a previously-shared database unavailable.
     */
    private static native boolean unshareDB(long listener, String name);

    /**
     * A convenience that, given a filesystem path to a database, returns the database name
     * for use in an HTTP URI path.
     */
    private static native String uriNameFromPath(String path);
}
