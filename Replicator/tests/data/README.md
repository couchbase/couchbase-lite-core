# Replicator Test Data

This directory contains Sync Gateway configuration* and databases for use with LiteCore's `ReplicatorAPITests` unit tests, specifically those tests tagged with `[.RealReplicator]`. These currently don't run automatically, because they require a Sync Gateway to be running.

(Similarly, couchbase-lite-ios also has some disabled Objective-C unit tests that connect to this server.)

1. Install the latest build of Sync Gateway 2.0 (or later).
2. Open a command shell at this directory.
3. `sync_gateway config.json`

If you want to run a second Sync Gateway instance with SSL, to test SSL connections (there aren't currently any LiteCore tests for this, but there's a Couchbase Lite/iOS one), do this:

4. Open another shell at this directory
5. `sync_gateway ssl_config.json`

This uses a self-signed certificate. You can find a copy of the certificate at `sg_cert.pem`; the private key is `sg_key.pem`.

There is a third configuration file `client_ssl_config.json` for enabling TLS client certificate support in Sync Gateway, which is (as of November 2019) an in-progress unsupported feature.

>* **SECURITY WARNING:** The configuration file here opens the admin port (4985) to the network, to allow the unit tests to run on a different device (such as a phone) and still be able to erase server databases. This is **far too insecure** for use with anything other than test data! Don’t copy this config.json and use it for your own configurations, at least without removing the `adminInterface` property.

>* **HINT:** Sync Gateway will periodically write to the files in `walrus_data`, causing Git to show them as modified. To stop this minor annoyance, run `git update-index --assume-unchanged walrus_data/*.walrus`. (And to undo that, do it again but with `--no-assume-unchanged`.)

# HTTP Proxy Testing

Also in this directory is `tinyproxy.conf`, a configuration file for [tinyproxy](https://tinyproxy.github.io) that supports the unit tests in `RESTClientTest.cc`.

To install tinyproxy on macOS, just run `brew install tinyproxy`; for other platforms, see the tinyproxy website.

To start the proxy for testing, you can just `cd` to this directory and run `./tinyproxy.conf`, or else `tinyproxy -d -c tinyproxy.conf`.
