package com.couchbase.litecore;

/**
 * Configuration for a C4Listener.
 */
public class C4ListenerConfig {
    private int port;   ///< TCP port to listen on
    private int apis;   ///< Which API(s) to enable

    // For REST listeners only:
    private String directory;       ///< Directory where newly-PUT databases will be created
    private boolean allowCreateDBs; ///< If true, "PUT /db" is allowed
    private boolean allowDeleteDBs; ///< If true, "DELETE /db" is allowed

    // For sync listeners only:
    private boolean allowPush;
    private boolean allowPull;

    public C4ListenerConfig() {
    }

    public C4ListenerConfig(int port,
                            int apis,
                            String directory,
                            boolean allowCreateDBs,
                            boolean allowDeleteDBs,
                            boolean allowPush,
                            boolean allowPull) {
        this.port = port;
        this.apis = apis;
        this.directory = directory;
        this.allowCreateDBs = allowCreateDBs;
        this.allowDeleteDBs = allowDeleteDBs;
        this.allowPush = allowPush;
        this.allowPull = allowPull;
    }

    public int getPort() {
        return port;
    }

    public void setPort(int port) {
        this.port = port;
    }

    public int getApis() {
        return apis;
    }

    public void setApis(int apis) {
        this.apis = apis;
    }

    public String getDirectory() {
        return directory;
    }

    public void setDirectory(String directory) {
        this.directory = directory;
    }

    public boolean isAllowCreateDBs() {
        return allowCreateDBs;
    }

    public void setAllowCreateDBs(boolean allowCreateDBs) {
        this.allowCreateDBs = allowCreateDBs;
    }

    public boolean isAllowDeleteDBs() {
        return allowDeleteDBs;
    }

    public void setAllowDeleteDBs(boolean allowDeleteDBs) {
        this.allowDeleteDBs = allowDeleteDBs;
    }

    public boolean isAllowPush() {
        return allowPush;
    }

    public void setAllowPush(boolean allowPush) {
        this.allowPush = allowPush;
    }

    public boolean isAllowPull() {
        return allowPull;
    }

    public void setAllowPull(boolean allowPull) {
        this.allowPull = allowPull;
    }

    @Override
    public String toString() {
        return "C4ListenerConfig{" +
                "port=" + port +
                ", apis=" + apis +
                ", directory='" + directory + '\'' +
                ", allowCreateDBs=" + allowCreateDBs +
                ", allowDeleteDBs=" + allowDeleteDBs +
                ", allowPush=" + allowPush +
                ", allowPull=" + allowPull +
                '}';
    }
}
