//
// CertificateTest.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Test.hh"
#include "c4.hh"
#include "PublicKey.hh"
#include "Certificate.hh"
#include <iostream>

using namespace litecore::crypto;
using namespace std;


static constexpr const char* kSubjectName = "CN=Jane Doe, O=ExampleCorp, C=US, "
                                            "emailAddress=jane@example.com";

static constexpr const char* kCAName = "CN=TrustMe Root CA, O=TrustMe Corp., C=US";


TEST_CASE("Key generation", "[Certs]") {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    CHECK(key->context() != nullptr);
    CHECK(key->description() == "2048-bit RSA private key");
    auto data = key->publicKeyData(KeyFormat::DER);
    CHECK(data.size == 294);
    cerr << "Raw data: " << data << '\n';
    cerr << "PEM data:\n" << key->publicKeyData(KeyFormat::PEM) << '\n';

    Retained<PublicKey> publicKey = key->publicKey();
    CHECK(publicKey->description() == "2048-bit RSA public key");
    CHECK(publicKey->data(KeyFormat::DER) == data);

    publicKey = new PublicKey(data);
    CHECK(publicKey->description() == "2048-bit RSA public key");
    CHECK(publicKey->data(KeyFormat::DER) == data);
}


TEST_CASE("Self-signed cert generation", "[Certs]") {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600*24;
    Retained<Cert> cert = new Cert(kSubjectName, issuerParams, key);

    cerr << "Subject: " << cert->subjectName() << "\n";
    cerr << "Info:\n" << cert->info("\t");

    alloc_slice data = cert->data();
    cerr << "Raw data: " << cert->data() << '\n';
    cerr << "PEM data:\n" << string(cert->data(KeyFormat::PEM)) << '\n';

    CHECK(cert->subjectName() == kSubjectName);
    cert = nullptr;

    cert = new Cert(data);
    CHECK(cert->subjectName() == kSubjectName);
}


TEST_CASE("Persistent key and cert", "[Certs]") {
    Retained<PersistentPrivateKey> key = PersistentPrivateKey::generateRSA(2048, "LiteCoreTest");
    cerr << "Public key data: " << key->publicKeyData(KeyFormat::DER) << "\n";
    Retained<PublicKey> pubKey = key->publicKey();
    CHECK(pubKey != nullptr);
    
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600*24;
    Retained<Cert> cert = new Cert(kSubjectName, issuerParams, key);

    cert->makePersistent();

    Retained<Cert> cert2 = Cert::load(pubKey);
    CHECK(cert2);
    CHECK(cert2->data() == cert->data());
}


TEST_CASE("Cert request", "[Certs]") {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Retained<CertSigningRequest> csr = new CertSigningRequest(kSubjectName, key);
    CHECK(csr->subjectName() == kSubjectName);
    CHECK(csr->subjectPublicKey()->data(KeyFormat::Raw) == key->publicKey()->data(KeyFormat::Raw));

    alloc_slice data = csr->data();
    alloc_slice pemData = csr->data(KeyFormat::PEM);
    cerr << "Raw data: " << csr->data() << '\n';
    cerr << "PEM data:\n" << string(pemData) << '\n';

    // Reconstitute it from data:
    Retained<CertSigningRequest> csr2 = new CertSigningRequest(pemData);
    CHECK(csr2->data() == data);
    CHECK(csr2->data(KeyFormat::PEM) == pemData);
    CHECK(csr2->subjectName() == kSubjectName);
    CHECK(csr2->subjectPublicKey()->data(KeyFormat::Raw) == key->publicKey()->data(KeyFormat::Raw));

    // Create a CA cert:
    Retained<PrivateKey> caKey = PrivateKey::generateTemporaryRSA(2048);
    Cert::IssuerParameters caIssuerParams;
    caIssuerParams.is_ca = true;
    Retained<Cert> caCert = new Cert(kCAName, caIssuerParams, caKey);
    cerr << "CA cert info:\n" << caCert->info("\t");

    // Sign it:
    Cert::IssuerParameters caClientParams;
    caClientParams.validity_secs = 3600*24;
    Retained<Cert> clientCert = csr2->sign(caClientParams, caKey, caCert);

    cerr << "Client cert info:\n" << clientCert->info("\t");
}
