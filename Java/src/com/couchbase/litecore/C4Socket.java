package com.couchbase.litecore;


import android.util.Log;

import java.net.URI;
import java.net.URISyntaxException;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

import static com.couchbase.litecore.Constants.C4ErrorDomain.POSIXDomain;
import static com.couchbase.litecore.Constants.C4ErrorDomain.WebSocketDomain;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2Scheme;
import static com.couchbase.litecore.C4Replicator.kC4Replicator2TLSScheme;

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
    private WebSocket webSocket = null;
    private static C4Socket c4sock = null;

    //-------------------------------------------------------------------------
    // constructor
    //-------------------------------------------------------------------------
    public C4Socket(long handle) {
        this.handle = handle;
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

    private static void open(long socket, String scheme, String hostname, int port, String path) {
        Log.e(TAG, "C4Socket.callback.open() socket -> 0x" + Long.toHexString(socket) + ", scheme -> " + scheme + ", hostname -> " + hostname + ", port -> " + port + ", path -> " + path);

        c4sock = new C4Socket(socket);

        reverseLookupTable.put(socket, c4sock);

        OkHttpClient client = new OkHttpClient.Builder()
                .connectTimeout(10, TimeUnit.SECONDS)
                .readTimeout(30, TimeUnit.SECONDS)
                .writeTimeout(30, TimeUnit.SECONDS)
                .build();

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

        Request request = new Request.Builder()
                .url(uri.toString())
                .build();

        client.newWebSocket(request, c4sock);
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
}
