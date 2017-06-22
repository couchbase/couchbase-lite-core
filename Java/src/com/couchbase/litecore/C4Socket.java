package com.couchbase.litecore;


import android.os.Build;
import android.util.Log;

import com.couchbase.litecore.fleece.FLValue;

import java.io.IOException;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;

import okhttp3.Authenticator;
import okhttp3.Challenge;
import okhttp3.Credentials;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.Route;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

import static com.couchbase.lite.ReplicatorConfiguration.kCBLReplicatorAuthOption;
import static com.couchbase.lite.ReplicatorConfiguration.kCBLReplicatorAuthPassword;
import static com.couchbase.lite.ReplicatorConfiguration.kCBLReplicatorAuthUserName;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2Scheme;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2TLSScheme;
import static com.couchbase.litecore.Constants.C4ErrorDomain.POSIXDomain;
import static com.couchbase.litecore.Constants.C4ErrorDomain.WebSocketDomain;

public class C4Socket extends WebSocketListener {

    //-------------------------------------------------------------------------
    // Constants
    //-------------------------------------------------------------------------
    private static final String TAG = C4Socket.class.getSimpleName();
    public static final String WEBSOCKET_SCHEME = "ws";
    public static final String WEBSOCKET_SECURE_CONNECTION_SCHEME = "wss";

    //-------------------------------------------------------------------------
    // Static Variables
    //-------------------------------------------------------------------------
    // Long: handle of C4Socket native address
    // C4Socket: Java class holds handle
    private static Map<Long, C4Socket> reverseLookupTable
            = Collections.synchronizedMap(new HashMap<Long, C4Socket>());

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Socket
    private URI uri = null;
    private Map<String, Object> options;
    private WebSocket webSocket = null;
    private static C4Socket c4sock = null;
    OkHttpClient httpClient = null;

    //-------------------------------------------------------------------------
    // constructor
    //-------------------------------------------------------------------------
    public C4Socket(long handle, URI uri, Map<String, Object> options) {
        this.handle = handle;
        this.uri = uri;
        this.options = options;
        this.httpClient = setupOkHttpClient();
    }

    //-------------------------------------------------------------------------
    // WebSocketListener interface
    //-------------------------------------------------------------------------

    @Override
    public void onOpen(WebSocket webSocket, Response response) {
        Log.e(TAG, "WebSocketListener.onOpen() response -> " + response);
        this.webSocket = webSocket;
        opened(handle);
    }

    @Override
    public void onMessage(WebSocket webSocket, String text) {
        Log.e(TAG, "WebSocketListener.onMessage() text -> " + text);
        received(handle, text.getBytes());
    }

    @Override
    public void onMessage(WebSocket webSocket, ByteString bytes) {
        Log.e(TAG, "WebSocketListener.onMessage() bytes -> " + bytes.hex());
        received(handle, bytes.toByteArray());
    }

    @Override
    public void onClosing(WebSocket webSocket, int code, String reason) {
        Log.e(TAG, "WebSocketListener.onClosing() code -> " + code + ", reason -> " + reason);
        closeRequested(handle, code, reason);
    }

    @Override
    public void onClosed(WebSocket webSocket, int code, String reason) {
        Log.e(TAG, "WebSocketListener.onClosed() code -> " + code + ", reason -> " + reason);
        closed(handle, WebSocketDomain, code);
    }

    @Override
    public void onFailure(WebSocket webSocket, Throwable t, Response response) {
        Log.e(TAG, "WebSocketListener.onFailure() response -> " + response, t);

        // Invoked when a web socket has been closed due to an error reading from or writing to the
        // network. Both outgoing and incoming messages may have been lost. No further calls to this
        // listener will be made.

        if (response != null)
            closed(handle, WebSocketDomain, response.code());
        else if (t != null) {
            // TODO: Following codes works with only Android.
            if (t.getCause() != null && t.getCause().getCause() != null) {
                android.system.ErrnoException e = (android.system.ErrnoException) t.getCause().getCause();
                closed(handle, POSIXDomain, e != null ? e.errno : 0);
            } else {
                closed(handle, POSIXDomain, 0);
            }
        } else
            closed(handle, WebSocketDomain, 0);
    }


    //-------------------------------------------------------------------------
    // callback methods from JNI
    //-------------------------------------------------------------------------

    private static void open(long socket, String scheme, String hostname, int port, String path, byte[] optionsFleece) {
        Log.e(TAG, "C4Socket.callback.open() socket -> 0x" + Long.toHexString(socket) + ", scheme -> " + scheme + ", hostname -> " + hostname + ", port -> " + port + ", path -> " + path);
        Log.e(TAG, "optionsFleece: " + (optionsFleece != null ? "not null" : "null"));

        Map<String, Object> options = null;
        if (optionsFleece != null) {
            options = FLValue.fromData(optionsFleece).asDict();
            Log.e(TAG, "options = " + options);
        }

        // NOTE: OkHttp can not understand blip/blips
        if (scheme.equalsIgnoreCase(kC4Replicator2Scheme))
            scheme = WEBSOCKET_SCHEME;
        else if (scheme.equalsIgnoreCase(kC4Replicator2TLSScheme))
            scheme = WEBSOCKET_SECURE_CONNECTION_SCHEME;

        URI uri;
        try {
            uri = new URI(scheme, null, hostname, port, path, null, null);
        } catch (URISyntaxException e) {
            Log.e(TAG, "Error with instantiating URI", e);
            return;
        }

        c4sock = new C4Socket(socket, uri, options);
        reverseLookupTable.put(socket, c4sock);
        c4sock.start();
    }

    private static void write(long handle, byte[] allocatedData) {
        if (handle == 0 || allocatedData == null) {
            Log.e(TAG, "C4Socket.callback.write() parameter error");
            return;
        }

        Log.e(TAG, "C4Socket.callback.write() handle -> 0x" + Long.toHexString(handle) + ", allocatedData.length -> " + allocatedData.length);

        C4Socket socket = reverseLookupTable.get(handle);
        if (socket.webSocket.send(ByteString.of(allocatedData, 0, allocatedData.length)))
            completedWrite(handle, allocatedData.length);
        else
            Log.e(TAG, "C4Socket.callback.write() FAILED to send data");
    }

    private static void completedReceive(long handle, long byteCount) {
        Log.e(TAG, "C4Socket.callback.completedReceive() socket -> 0x" + Long.toHexString(handle) + ", byteCount -> " + byteCount);

        // NOTE: No further action is not required?
    }

    private static void close(long handle) {
        Log.e(TAG, "C4Socket.callback.close() socket -> 0x" + Long.toHexString(handle));

        // NOTE: close(long) method should not be called.
    }

    private static void requestClose(long handle, int status, String message) {
        Log.e(TAG, "C4Socket.callback.requestClose() socket -> 0x" + Long.toHexString(handle) + ", status -> " + status + ", message -> " + message);

        C4Socket socket = reverseLookupTable.get(handle);
        if (!socket.webSocket.close(status, message)) {
            Log.e(TAG, "C4Socket.callback.requestClose() Failed to attempt to initiate a graceful shutdown of this web socket.");
        }
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------
    public native static void registerFactory();

    private native static void opened(long socket);

    private native static void closed(long socket, int errorDomain, int errorCode);

    private native static void closeRequested(long socket, int status, String message);

    private native static void completedWrite(long socket, long byteCount);

    private native static void received(long socket, byte[] data);

    //-------------------------------------------------------------------------
    // private methods
    //-------------------------------------------------------------------------

    private OkHttpClient setupOkHttpClient() {
        OkHttpClient.Builder builder = new OkHttpClient.Builder();

        // timeouts
        builder.connectTimeout(10, TimeUnit.SECONDS)
                .readTimeout(30, TimeUnit.SECONDS)
                .writeTimeout(30, TimeUnit.SECONDS);

        // redirection
        builder.followRedirects(true).followSslRedirects(true);

        // authenticator
        Authenticator authenticator = setupAuthenticator();
        if (authenticator != null)
            builder.authenticator(authenticator);

        return builder.build();
    }

    private Authenticator setupAuthenticator() {
        if (options != null && options.containsKey(kCBLReplicatorAuthOption)) {
            Map<String, Object> auth = (Map<String, Object>) options.get(kCBLReplicatorAuthOption);
            if (auth != null) {
                final String username = (String) auth.get(kCBLReplicatorAuthUserName);
                final String password = (String) auth.get(kCBLReplicatorAuthPassword);
                if (username != null && password != null) {
                    return new Authenticator() {
                        @Override
                        public Request authenticate(Route route, Response response) throws IOException {

                            // http://www.ietf.org/rfc/rfc2617.txt

                            Log.i(TAG, "Authenticating for response: " + response);

                            // If failed 3 times, give up.
                            if (responseCount(response) >= 3)
                                return null;

                            List<Challenge> challenges = response.challenges();
                            Log.i(TAG, "Challenges: " + challenges);
                            if (challenges != null) {
                                for (Challenge challenge : challenges) {
                                    if (challenge.scheme().equals("Basic")) {
                                        String credential = Credentials.basic(username, password);
                                        return response.request().newBuilder().header("Authorization", credential).build();
                                    }

                                    // NOTE: Not implemented Digest authentication
                                    //       https://github.com/rburgst/okhttp-digest
                                    //else if(challenge.scheme().equals("Digest")){
                                    //}
                                }

                            }

                            return null;
                        }
                    };
                }
            }
        }
        return null;
    }

    private int responseCount(Response response) {
        int result = 1;
        while ((response = response.priorResponse()) != null) {
            result++;
        }
        return result;
    }

    private void start() {
        Log.i(TAG, String.format(Locale.ENGLISH, "C4Socket connecting to %s...", uri));
        httpClient.newWebSocket(newRequest(), c4sock);
    }

    private Request newRequest() {
        Request.Builder builder = new Request.Builder();

        // Sets the URL target of this request.
        builder.url(uri.toString());

        // Set/update the "Host" header:
        String host = uri.getHost();
        if (uri.getPort() != -1)
            host = String.format(Locale.ENGLISH, "%s:%d", host, uri.getPort());
        builder.addHeader("Host", host);

        // Add headers from requests
        // Note: As the request is created from plain URI, so no headers.

        // Add cookie headers from the cookie storage:
        // TODO: Implement when Cookie store is supported!

        // Add User-Agent if necessary:
        builder.addHeader("User-Agent", getUserAgent());

        return builder.build();
    }

    /**
     * Return User-Agent value
     * Format: ex: CouchbaseLite/1.2 (Java Linux/MIPS Android/)
     */
    public static String USER_AGENT = null;

    // TODO:
    public static String getUserAgent() {
        if (USER_AGENT == null) {
            USER_AGENT = String.format(Locale.ENGLISH, "%s/%s (%s/%s)",
                    "CouchbaseLite",
                    "2.0.0",
                    "Android",
                    Build.VERSION.RELEASE);
        }
        return USER_AGENT;
    }
}
