#!/bin/bash -e

# Copyright 2022-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.
#
# This is the script for generating a cert chain containing a root, an intermediate, and a leaf cert.
# The final output includes
#  - sg_cert.pem // Cert chain for SG config
#  - sg_key.pem  // Private key for SG config
#  - cert.pem    // Leaf cert copied from the cert chain for the pinned cert used in tests
# 
# Note: The script works was written for macOS,  and was tested against openSSL 2.8.3.

function usage 
{
  echo "Usage: ${0} -o <Output Directory>"
}

while [[ $# -gt 0 ]]
do
  key=${1}
  case $key in
      -o)
      OUTPUT_DIR=${2}
      shift
      ;;
      *)
      usage
      exit 3
      ;;
  esac
  shift
done

if [ -z "$OUTPUT_DIR" ]
then
  usage
  exit 4
fi

rm -rf ${OUTPUT_DIR}
mkdir -p ${OUTPUT_DIR}
pushd ${OUTPUT_DIR} > /dev/null

echo ">> Create openssl with v3_ca config ..."
cp /etc/ssl/openssl.cnf .
printf "\n[ v3_ca ]\nbasicConstraints = critical,CA:TRUE\nsubjectKeyIdentifier = hash\nauthorityKeyIdentifier = keyid:always,issuer:always" >> openssl.cnf

echo ">> Create root CA certificate ..."
openssl genrsa -out root.key 2048
openssl req -new -x509 -sha256 -days 3650 -config openssl.cnf -extensions v3_ca -key root.key -out root.pem -subj "/CN=Couchbase Root CA"

echo ">> Create intermediate certificate ..."
openssl genrsa -out inter.key 2048
openssl req -new -sha256 -key inter.key -out inter.csr -subj "/CN=Inter"
openssl x509 -req -sha256 -days 3650 -extfile openssl.cnf -extensions v3_ca -CA root.pem -CAkey root.key -CAcreateserial -in inter.csr -out inter.pem
rm -rf openssl.cnf

echo ">> Create leaf certificate ..."
openssl genrsa -out leaf.key 2048
openssl req -new -sha256 -key leaf.key -out leaf.csr -subj "/CN=localhost"
openssl x509 -req -sha256 -days 3650 -CA inter.pem -CAkey inter.key -CAcreateserial -in leaf.csr -out leaf.pem 

echo ">> Verify created certificate chain ..."
openssl verify -CAfile root.pem -untrusted inter.pem leaf.pem

echo ">> Create certs.pem (SG Config), prevkey.pem (SG Config), and cert.pem (Pinned Cert for testing) ..."
echo ">> Done"
cat leaf.pem inter.pem root.pem > sg_cert.pem
cp leaf.pem cert.pem
cp leaf.key sg_key.pem

rm -rf root.*
rm -rf inter.*
rm -rf leaf.*

popd > /dev/null
