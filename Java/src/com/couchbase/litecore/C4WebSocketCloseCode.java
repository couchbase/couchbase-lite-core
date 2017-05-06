package com.couchbase.litecore;

// Standard WebSocket close status codes, for use in C4Errors with WebSocketDomain.
// These are defined at <http://tools.ietf.org/html/rfc6455#section-7.4.1>
public interface C4WebSocketCloseCode {
    int kWebSocketCloseNormal = 1000;
    int kWebSocketCloseGoingAway = 1001; // Peer has to close, e.g. because host app is quitting
    int kWebSocketCloseProtocolError = 1002; // Protocol violation: invalid framing data
    int kWebSocketCloseDataError = 1003; // Message payload cannot be handled
    int kWebSocketCloseNoCode = 1005; // Never sent, only received
    int kWebSocketCloseAbnormal = 1006; // Never sent, only received
    int kWebSocketCloseBadMessageFormat = 1007; // Unparseable message
    int kWebSocketClosePolicyError = 1008;
    int kWebSocketCloseMessageTooBig = 1009;
    int kWebSocketCloseMissingExtension = 1010; // Peer doesn't provide a necessary extension
    int kWebSocketCloseCantFulfill = 1011; // Can't fulfill request due to "unexpected condition"
    int kWebSocketCloseTLSFailure = 1015; // Never sent, only received

    int kWebSocketCloseFirstAvailable = 4000; // First unregistered code for freeform use
}
