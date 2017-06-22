package com.couchbase.litecore;

import android.util.Log;

import com.couchbase.lite.CouchbaseLiteException;

import java.net.MalformedURLException;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URL;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import okhttp3.Credentials;
import okhttp3.Request;
import okhttp3.Response;

/**
 * NOTE: OkHttp supports redirection and handling authentication.
 * For now, HttpLogic implementation is not used.
 * Keep this implementation for a while.
 */
public class HttpLogic {
    private static final String TAG = HttpLogic.class.getSimpleName();

    private static final int kMaxRedirects = 10;

    private static final int OkHttpDomain = 100; // temporary

    private URI uri = null;

    // NOTE: OkHttp follows redirect as default.
    // https://square.github.io/okhttp/2.x/okhttp/com/squareup/okhttp/OkHttpClient.html#setFollowRedirects-boolean-
    private boolean handleRedirects = false;
    private boolean shouldContinue = false;
    private boolean shouldRetry = false;
    private String credentials = null;
    private int httpStatus = 0;
    private CouchbaseLiteException error = null;

    private String authorizationHeader = null;
    private Response response = null;
    private int redirectCount = 0;

    //---------------------------------------------
    // Constructors
    //---------------------------------------------

    public HttpLogic(URI uri) {
        this.uri = uri;
    }

    //---------------------------------------------
    // Getters and Setters
    //---------------------------------------------

    public URI getUri() {
        return uri;
    }

    public boolean isHandleRedirects() {
        return handleRedirects;
    }

    public void setHandleRedirects(boolean handleRedirects) {
        this.handleRedirects = handleRedirects;
    }

    public boolean isShouldContinue() {
        return shouldContinue;
    }

    public boolean isShouldRetry() {
        return shouldRetry;
    }

    public void setCredential(String username, String password) {
        credentials = Credentials.basic(username, password);
    }

    public String getCredentials() {
        return credentials;
    }

    public void setCredentials(String credentials) {
        this.credentials = credentials;
    }

    public int getHttpStatus() {
        return httpStatus;
    }

    public CouchbaseLiteException getError() {
        return error;
    }

    public Request newRequest() {
        Request.Builder builder = new Request.Builder();

        // Sets the URL target of this request.
        builder.url(uri.toString());

        // Set/update the "Host" header:

        String host = uri.getHost();
        if (uri.getPort() != -1)
            host = String.format(Locale.ENGLISH, "%s:%d", host, uri.getPort());
        builder.addHeader("Host", host);

        // Create the CFHTTPMessage:

        // Add cookie headers from the NSHTTPCookieStorage:

        // Add User-Agent if necessary:


        // If this is a retry, set auth headers from the credential we got:
        if (response != null && credentials != null) {
            builder.addHeader("Authorization", credentials);
        }

        Request request = builder.build();

        authorizationHeader = request.header("Authorization");
        shouldContinue = shouldRetry = false;
        httpStatus = 0;

        return request;
    }

    public void receivedResponse(Response res) {
        if (res == response)
            return;

        response = res;

        shouldContinue = shouldRetry = false;
        httpStatus = response.code();
        switch (httpStatus) {
            case 301:
            case 302:
            case 307: {
                // NOTE: OkHttp follows redirect as default.
                // Redirect
                if (!handleRedirects)
                    break;
                if (++redirectCount > kMaxRedirects)
                    setError(-1007, null); // TODO: NSURLErrorHTTPTooManyRedirects = -1007
                else if (!redirect())
                    setError(-1010, null); // TODO: NSURLErrorRedirectToNonExistentLocation = -1010
                else
                    shouldRetry = true;
                break;
            }

            case 401:
            case 407: {
                String authResponse = response.header("WWW-Authenticate");
                if (authorizationHeader == null) {
                    if (credentials == null)
                        credentials = credentialForAuthHeader(authResponse);
                    Log.i(TAG, String.format(Locale.ENGLISH, "%s: Auth challenge; credential = %s", this, credentials));
                    if (credentials != null) {
                        // Recoverable auth failure -- try again with new _credential:
                        shouldRetry = true;
                        break;
                    }
                }
                Log.i(TAG, String.format(Locale.ENGLISH,
                        "%s: HTTP auth failed; sent Authorization: %s  ;  got WWW-Authenticate: %s",
                        this, authorizationHeader, authResponse));
                Map<String, String> challengeInfo = parseAuthHeader(authResponse);

                Map<String, Object> errorInfo = new HashMap<>();
                if (authorizationHeader != null)
                    errorInfo.put("HTTPAuthorization", authorizationHeader);
                if (authResponse != null)
                    errorInfo.put("HTTPAuthenticateHeader", authResponse);
                if (challengeInfo != null)
                    errorInfo.put("AuthChallenge", challengeInfo);
                setError(-1013, errorInfo); // TODO: NSURLErrorUserAuthenticationRequired = -1013
                break;
            }

            default: {
                if (httpStatus < 300)
                    shouldContinue = true;
                break;
            }
        }
    }

    private String credentialForAuthHeader(String authHeader) {
        // Basic & digest auth: http://www.ietf.org/rfc/rfc2617.txt
        Map<String, String> challenge = parseAuthHeader(authHeader);
        if (challenge == null || challenge.size() == 0)
            return null;

        // Get the auth type:
        String authenticationMethod;
        String scheme = challenge.get("Scheme");
        if (scheme != null && scheme.equals("Basic"))
            authenticationMethod = "Basic"; // NOTE: temporary
        else if (scheme != null && scheme.equals("Digest"))
            authenticationMethod = "Digest"; // NOTE: temporary
        else
            return null;

        // Get the realm:
        String realm = challenge.get("realm");
        if (realm == null)
            return null;

        String cred = null;
        cred = credentialForRealm(realm, authenticationMethod);
        return cred;
    }

    private String credentialForRealm(String realm, String authenticationMethod) {
        // TODO
        return null;
    }

    private static Pattern re = Pattern.compile("(\\w+)\\s+(\\w+)=((\\w+)|\"([^\"]+))");

    /**
     * BLIPHTTPLogic.m
     * + (NSDictionary*) parseAuthHeader: (NSString*)authHeader
     *
     * @param authHeader
     * @return
     */
    private static Map<String, String> parseAuthHeader(String authHeader) {
        if (authHeader == null || authHeader.length() == 0)
            return null;
        Map<String, String> challenge = new HashMap<String, String>();
        Matcher m = re.matcher(authHeader);
        while (m.find()) {
            String scheme = m.group(1);
            String key = m.group(2);
            String value = m.group(4);
            if (value == null || value.length() == 0)
                value = m.group(5);
            challenge.put(key, value);
            challenge.put("Scheme", scheme);
        }
        challenge.put("WWW-Authenticate", authHeader);
        return challenge;
    }

    private boolean redirect() {
        String location = response.header("Location");
        if (location == null)
            return false;
        URL url = null;
        try {
            url = new URL(uri.toURL(), location);
        } catch (MalformedURLException e) {
            return false;
        }
        if (!url.getProtocol().equalsIgnoreCase("http") && !url.getProtocol().equalsIgnoreCase("https"))
            return false;
        try {
            uri = url.toURI();
        } catch (URISyntaxException e) {
            return false;
        }
        return true;
    }

    private void setError(int code, Map<String, Object> userInfo) {
        Map<String, Object> info = new HashMap<>();
        info.put("NSURLErrorFailingURLErrorKey", uri); //TODO
        if (userInfo != null)
            info.putAll(userInfo);
        error = new CouchbaseLiteException(OkHttpDomain, code, info);
    }
}
