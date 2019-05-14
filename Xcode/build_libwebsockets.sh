#! /bin/bash -e
# This script builds the libwebsockets submodule.
# It's run by Xcode when building the target LiteCoreWebSocket.

cd "$SRCROOT/../vendor/libwebsockets"
mkdir -p build
cd build
cmake .. -DLWS_WITHOUT_SERVER=1 \
         -DLWS_WITH_HTTP2=0 \
         -DLWS_WITH_SHARED=0 \
         -DLWS_WITH_LEJP=0 -DLWS_WITH_STRUCT_JSON=0 \
         -DLWS_WITHOUT_TESTAPPS=1 -DLWS_WITHOUT_TEST_PING=1 -DLWS_WITHOUT_TEST_CLIENT=1\
         -DLWS_WITHOUT_TEST_SERVER=1 -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=1 \
         -DLWS_WITH_MBEDTLS=1 \
         -DLWS_MBEDTLS_INCLUDE_DIRS=../../mbedtls/build/include \
         -DMBEDCRYPTO_LIBRARY=../../mbedtls/build/library/libmbedcrypto.a \
         -DMBEDTLS_LIBRARY=../../mbedtls/build/library/libmbedtls.a \
         -DMBEDX509_LIBRARY=../../mbedtls/build/library/libmbedx509.a
make
