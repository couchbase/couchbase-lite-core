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


static constexpr const slice kSubjectName = "CN=Jane Doe, O=ExampleCorp, C=US, "
                                            "emailAddress=jane@example.com"_sl;
static constexpr const slice kSubject2Name = "CN=Richard Roe, O=ExampleCorp, C=US, "
                                            "emailAddress=dick@example.com"_sl;

static constexpr const slice kCAName = "CN=TrustMe Root CA, O=TrustMe Corp., C=US"_sl;


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


static pair<Retained<PrivateKey>,Retained<Cert>> makeCert(slice subjectName) {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600*24;
    return {key, new Cert(subjectName, issuerParams, key)};
}


TEST_CASE("Self-signed cert generation", "[Certs]") {
    auto [key, cert] = makeCert(kSubjectName);
    cerr << "Subject: " << cert->subjectName() << "\n";
    cerr << "Info:\n" << cert->summary("\t");

    alloc_slice data = cert->data();
    cerr << "Raw data: " << cert->data() << '\n';
    cerr << "PEM data:\n" << string(cert->data(KeyFormat::PEM)) << '\n';

    CHECK(cert->subjectName() == kSubjectName);
    cert = nullptr;

    cert = new Cert(data);
    CHECK(cert->subjectName() == kSubjectName);
}


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
TEST_CASE("Persistent key and cert", "[Certs]") {
    Retained<PersistentPrivateKey> key = PersistentPrivateKey::generateRSA(2048);
    cerr << "Public key raw data: " << key->publicKeyData(KeyFormat::Raw) << "\n";
    auto pubKeyData = key->publicKeyData(KeyFormat::DER);
    cerr << "Public key DER data: " << pubKeyData << "\n";
    Retained<PublicKey> pubKey = key->publicKey();
    CHECK(pubKey != nullptr);
    CHECK(pubKey->data(KeyFormat::Raw) == key->publicKeyData(KeyFormat::Raw));
    CHECK(pubKey->data(KeyFormat::DER) == key->publicKeyData(KeyFormat::DER));
    CHECK(pubKey->data(KeyFormat::PEM) == key->publicKeyData(KeyFormat::PEM));

    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600*24;
    Retained<Cert> cert = new Cert(kSubjectName, issuerParams, key);

    cert->makePersistent();

    Retained<Cert> cert2 = Cert::load(pubKey);
    CHECK(cert2);
    CHECK(cert2->data() == cert->data());

    // Try reloading from cert:
    key = cert->loadPrivateKey();
    REQUIRE(key);
    CHECK(pubKey->data(KeyFormat::Raw) == key->publicKeyData(KeyFormat::Raw));
    CHECK(pubKey->data(KeyFormat::DER) == key->publicKeyData(KeyFormat::DER));
    CHECK(pubKey->data(KeyFormat::PEM) == key->publicKeyData(KeyFormat::PEM));
}
#endif


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
    cerr << "CA cert info:\n" << caCert->summary("\t");

    // Sign it:
    Cert::IssuerParameters caClientParams;
    caClientParams.validity_secs = 3600*24;
    Retained<Cert> clientCert = csr2->sign(caClientParams, caKey, caCert);

    cerr << "Client cert info:\n" << clientCert->summary("\t");
}


TEST_CASE("Cert concatenation") {
    alloc_slice pem;
    {
        auto [key1, cert1] = makeCert(kSubjectName);
        auto [key2, cert2] = makeCert(kSubject2Name);
        CHECK(!cert1->hasChain());
        CHECK(!cert1->next());
        CHECK(cert1->dataOfChain() == cert1->data(KeyFormat::PEM));

        cert1->append(cert2);

        cerr << string(cert1->summary()) << "\n";
        cerr << string(cert2->summary()) << "\n";

        CHECK(cert1->hasChain());
        CHECK(!cert2->hasChain());
        CHECK(!cert2->next());
        REQUIRE(cert1->next() == cert2);

        // Convert to PEM:
        pem = cert1->dataOfChain();
        cerr << string(pem) << "\n";

        // Release 2nd cert in chain, then access it again:
        cerr << "Freeing cert2\n";
        cert2 = nullptr;

        CHECK(cert1->hasChain());
        auto next = cert1->next();
        REQUIRE(next);
        CHECK(!next->hasChain());
        CHECK(cert1->next() == next);
        CHECK(cert1->dataOfChain() == pem);
        cerr << "Done\n";
    }

    // Reconstitute both certs from the saved PEM data:
    Retained<Cert> cert = new Cert(pem);
    CHECK(cert->hasChain());
    auto next = cert->next();
    REQUIRE(next);
    CHECK(!next->hasChain());
    CHECK(cert->next() == next);
    CHECK(cert->dataOfChain() == pem);
    CHECK(cert->subjectName() == kSubjectName);
    CHECK(next->subjectName() == kSubject2Name);
}
