//
// CertificateTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4CppUtils.hh"
#include "PublicKey.hh"
#include "Certificate.hh"
#include "CertRequest.hh"
#include "Error.hh"
#include "LiteCoreTest.hh"
#include <iostream>


using namespace litecore;
using namespace litecore::crypto;
using namespace std;
using namespace fleece;


static constexpr const slice kSubjectName = "CN=Jane Doe, O=ExampleCorp, C=US, "
                                            "pseudonym=3Jane"_sl;
static constexpr const slice kSubject2Name = "CN=Richard Roe, O=ExampleCorp, C=US, ST=AZ"_sl;

static constexpr const slice kCAName = "CN=TrustMe Root CA, O=TrustMe Corp., C=US"_sl;


TEST_CASE("Creating subject names", "[Certs]") {
    DistinguishedName name({
        {"CN"_sl, "Jane Doe"_sl}, {"O"_sl, "ExampleCorp"_sl}, {"C"_sl, "US"_sl},
        {"pseudonym"_sl, "3Jane"_sl}});
    CHECK(name == kSubjectName);

    CHECK(name["CN"_sl] == "Jane Doe"_sl);
    CHECK(name["O"_sl] == "ExampleCorp"_sl);
    CHECK(name["C"_sl] == "US"_sl);
    CHECK(name["pseudonym"_sl] == "3Jane"_sl);
    CHECK(name["foo"_sl] == nullslice);

    name = DistinguishedName({
        {"CN"_sl, "Jane Doe, MD"_sl}});
    CHECK(name == "CN=Jane Doe\\, MD"_sl);
    CHECK(name["CN"_sl] == "Jane Doe, MD"_sl);
    CHECK(name["foo"_sl] == nullslice);

    name = DistinguishedName({
        {"CN"_sl, ",Jane,,Doe,"_sl}});
    CHECK(name == "CN=\\,Jane\\,\\,Doe\\,"_sl);
    CHECK(name["CN"_sl] == ",Jane,,Doe,"_sl);
    CHECK(name["foo"_sl] == nullslice);

    name = DistinguishedName("CN=Zegpold"_sl);
    CHECK(name["CN"_sl] == "Zegpold"_sl);
    CHECK(name["foo"_sl] == nullslice);

    name = DistinguishedName("CN=Zegpold\\, Jr,O=Example\\, Inc.,   OU=Mailroom"_sl);
    CHECK(name["CN"_sl] == "Zegpold, Jr"_sl);
    CHECK(name["O"_sl] == "Example, Inc."_sl);
    CHECK(name["OU"_sl] == "Mailroom"_sl);
    CHECK(name["foo"_sl] == nullslice);
}


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

static constexpr int kValidSeconds = 3600*24;

static pair<Retained<PrivateKey>,Retained<Cert>> makeCert(slice subjectName) {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = kValidSeconds;
    return {key, new Cert(DistinguishedName(subjectName), issuerParams, key)};
}


TEST_CASE("Self-signed cert generation", "[Certs]") {
    Retained<PrivateKey> key;
    Retained<Cert> cert;
    tie(key, cert) = makeCert(kSubjectName);
    cerr << "Subject: " << cert->subjectName() << "\n";
    cerr << "Info:\n" << cert->summary("\t");

    alloc_slice data = cert->data();
    cerr << "Raw data: " << cert->data() << '\n';
    cerr << "PEM data:\n" << string(cert->data(KeyFormat::PEM)) << '\n';

    CHECK(cert->isSigned());
    CHECK(cert->isSelfSigned());
    CHECK(cert->subjectName() == kSubjectName);

    time_t created, expires;
    tie(created, expires) = cert->validTimespan();
    time_t now = time(nullptr);
    CHECK(difftime(now, created) >= 0);
    CHECK(difftime(now, created) <= 100);
    CHECK(difftime(expires, created) == kValidSeconds);

    cert = nullptr;
    cert = new Cert(data);
    CHECK(cert->subjectName() == kSubjectName);
}


TEST_CASE("Self-signed cert with Subject Alternative Name", "[Certs]") {
    Cert::SubjectParameters subjectParams(DistinguishedName("CN=Jane Doe, O=ExampleCorp, C=US"_sl));
    subjectParams.subjectAltNames.emplace_back(SANTag::kRFC822Name,
                                                       "jane@example.com"_sl);
    subjectParams.subjectAltNames.emplace_back(SANTag::kDNSName,
                                                       "https://example.com/jane/"_sl);
    subjectParams.nsCertType = NSCertType(SSL_CLIENT | EMAIL);
    Cert::IssuerParameters issuerParams;
    issuerParams.validity_secs = 3600*24;
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Retained<Cert> cert;

    SECTION("Self-signed") {
        cert = new Cert(subjectParams, issuerParams, key);
    }
    SECTION("Issuer-signed") {
        Retained<CertSigningRequest> csr = new CertSigningRequest(subjectParams, key);
        cert = csr->sign(issuerParams, key);
    }

    CHECK(cert->nsCertType() == subjectParams.nsCertType);
    auto names = cert->subjectAltNames();
    REQUIRE(names.size() == 2);
    CHECK(names[0].first == SANTag::kRFC822Name);
    CHECK(names[0].second == "jane@example.com"_sl);
    CHECK(names[1].first == SANTag::kDNSName);
    CHECK(names[1].second == "https://example.com/jane/"_sl);
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
    Retained<Cert> cert = new Cert(DistinguishedName(kSubjectName), issuerParams, key);
    
    // Try to get private key with public key:
    key = PersistentPrivateKey::withPublicKey(pubKey);
    REQUIRE(key);
    CHECK(pubKey->data(KeyFormat::Raw) == key->publicKeyData(KeyFormat::Raw));
    CHECK(pubKey->data(KeyFormat::DER) == key->publicKeyData(KeyFormat::DER));
    CHECK(pubKey->data(KeyFormat::PEM) == key->publicKeyData(KeyFormat::PEM));
    
    // Try reloading from cert:
    key = cert->loadPrivateKey();
    REQUIRE(key);
    CHECK(pubKey->data(KeyFormat::Raw) == key->publicKeyData(KeyFormat::Raw));
    CHECK(pubKey->data(KeyFormat::DER) == key->publicKeyData(KeyFormat::DER));
    CHECK(pubKey->data(KeyFormat::PEM) == key->publicKeyData(KeyFormat::PEM));
    
    // Try a CSR:
    Retained<CertSigningRequest> csr = new CertSigningRequest(DistinguishedName(kSubjectName), key);
    CHECK(csr->subjectName() == kSubjectName);
    CHECK(csr->subjectPublicKey()->data(KeyFormat::Raw) == key->publicKey()->data(KeyFormat::Raw));
    alloc_slice data = csr->data();
    Retained<CertSigningRequest> csr2 = new CertSigningRequest(data);
    CHECK(csr2->subjectName() == kSubjectName);
    CHECK(csr2->subjectPublicKey()->data(KeyFormat::Raw) == key->publicKey()->data(KeyFormat::Raw));
    
    // CBL-1036: Remove a left over cert that causes test failures on some machines.
    Cert::deleteCert("Jane Doe");
    CHECK(Cert::loadCert("Jane Doe").get() == nullptr);
    
    // Delete the cert to cleanup:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Save the cert:
    cert->save("cert1", true);
    
    // Load the cert with the persistent ID:
    Retained<Cert> certA = Cert::loadCert("cert1");
    REQUIRE(certA);
    CHECK(certA->data() == cert->data());
    
    // Load the cert with public key:
    Retained<Cert> certB= Cert::load(pubKey);
    REQUIRE(certB);
    CHECK(certB->data() == cert->data());
    
    // Delete the cert:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Save and load again after delete:
    cert->save("cert1", true);
    Retained<Cert> certC = Cert::loadCert("cert1");
    REQUIRE(certA);
    CHECK(certA->data() == cert->data());
    
    // Delete the cert
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);

    // Delete the key
    key->remove();
}


TEST_CASE("Persistent save duplicate cert or id", "[Certs]") {
    // Create a keypair and a cert1:
    Retained<PersistentPrivateKey> key1 = PersistentPrivateKey::generateRSA(2048);
    Retained<PublicKey> pubKey = key1->publicKey();
    CHECK(pubKey != nullptr);

    Cert::IssuerParameters issuerParams1;
    issuerParams1.serial = "1"_sl;
    issuerParams1.validity_secs = 3600*24;
    Retained<Cert> cert1 = new Cert(DistinguishedName(kSubjectName), issuerParams1, key1);

    // CBL-1036: Remove a left over cert that causes test failures on some machines.
    Cert::deleteCert("Jane Doe");
    CHECK(Cert::loadCert("Jane Doe").get() == nullptr);
    
    // Delete cert1 to cleanup:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Save cert1:
    cert1->save("cert1", true);
    Retained<Cert> cert1a = Cert::loadCert("cert1");
    REQUIRE(cert1a);
    CHECK(cert1a->data() == cert1->data());
    
    // Save cert1 again with the same id:
    ExpectException(error::LiteCore, error::CryptoError, [&]{
        cert1->save("cert1", true);
    });
        
#ifdef __APPLE__
    // Save cert1 again with a different id:
    ExpectException(error::LiteCore, error::CryptoError, [&]{
        cert1->save("cert2", true);
    });
#endif
    
    // Create another keypair and cert2:
    Retained<PersistentPrivateKey> key2 = PersistentPrivateKey::generateRSA(2048);
    Retained<PublicKey> pubKey2 = key2->publicKey();
    CHECK(pubKey2 != nullptr);

    Cert::IssuerParameters issuerParams2;
    issuerParams2.serial = "2"_sl;
    issuerParams2.validity_secs = 3600*24;
    Retained<Cert> cert2 = new Cert(DistinguishedName(kSubject2Name), issuerParams2, key1);
    
    // Delete cert2 to cleanup:
    Cert::deleteCert("cert2");
    
    // Save cert2 to an existing ID:
    ExpectException(error::LiteCore, error::CryptoError, [&]{
        cert2->save("cert1", true);
    });
    
    // Save cert2:
    cert2->save("cert2", true);
    Retained<Cert> cert2a = Cert::loadCert("cert2");
    REQUIRE(cert2a);
    CHECK(cert2a->data() == cert2->data());
    
    // Delete cert1:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Delete cert2:
    Cert::deleteCert("cert2");
    CHECK(Cert::loadCert("cert2").get() == nullptr);

    // Delete keys
    key1->remove();
    key2->remove();
}


TEST_CASE("Persistent cert chain", "[Certs]") {
    // Create a CA Cert:
    Retained<PrivateKey> caKey = PrivateKey::generateTemporaryRSA(2048);
    Cert::IssuerParameters caIssuerParams;
    caIssuerParams.is_ca = true;
    Retained<Cert> caCert = new Cert(DistinguishedName(kCAName), caIssuerParams, caKey);
    cerr << "CA cert info:\n" << string(caCert->summary("\t"));
    
    // Create CSR1:
    Retained<PrivateKey> key1 = PrivateKey::generateTemporaryRSA(2048);
    Retained<CertSigningRequest> csr1 = new CertSigningRequest(DistinguishedName(kSubjectName), key1);
    CHECK(csr1->subjectName() == kSubjectName);
    CHECK(csr1->subjectPublicKey()->data(KeyFormat::Raw) == key1->publicKey()->data(KeyFormat::Raw));
    
    // Sign and create cert1 with the CA Cert:
    Cert::IssuerParameters caClientParams1;
    caClientParams1.serial = "1"_sl;
    caClientParams1.validity_secs = 3600*24;
    Retained<Cert> cert1 = csr1->sign(caClientParams1, caKey, caCert);
    cerr << "Cert1 info:\n" << string(cert1->summary("\t"));
    
    // Delete cert1 to cleanup:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Save cert1:
    cert1->save("cert1", true);
    Retained<Cert> cert1a = Cert::loadCert("cert1");
    REQUIRE(cert1a);
    CHECK(cert1a->data() == cert1->data());
    
    // Create CSR2:
    Retained<PrivateKey> key2 = PrivateKey::generateTemporaryRSA(2048);
    Retained<CertSigningRequest> csr2 = new CertSigningRequest(DistinguishedName(kSubject2Name), key2);
    CHECK(csr2->subjectName() == kSubject2Name);
    CHECK(csr2->subjectPublicKey()->data(KeyFormat::Raw) == key2->publicKey()->data(KeyFormat::Raw));
    
    // Sign and create cert2 with the same CA Cert as the cert1:
    Cert::IssuerParameters caClientParams2;
    caClientParams2.serial = "2"_sl;
    caClientParams2.validity_secs = 3600*24;
    Retained<Cert> cert2 = csr2->sign(caClientParams2, caKey, caCert);
    cerr << "Cert2 info:\n" << string(cert2->summary("\t"));
    
    // Delete cert2 to cleanup:
    Cert::deleteCert("cert2");
    CHECK(Cert::loadCert("cert2").get() == nullptr);
    
    // Save cert2:
    cert2->save("cert2", true);
    Retained<Cert> cert2a = Cert::loadCert("cert2");
    REQUIRE(cert2a);
    CHECK(cert2a->data() == cert2->data());
    
    // Load cert1 again to make sure that it's still loaded:
    Retained<Cert> cert1b = Cert::loadCert("cert1");
    REQUIRE(cert1b);
    CHECK(cert1b->data() == cert1->data());
    
    // Delete cert1:
    Cert::deleteCert("cert1");
    CHECK(Cert::loadCert("cert1").get() == nullptr);
    
    // Load cert2 again to make sure that it's still loaded:
    Retained<Cert> cert2b = Cert::loadCert("cert2");
    REQUIRE(cert2b);
    CHECK(cert2b->data() == cert2->data());
    
    // Delete cert2:
    Cert::deleteCert("cert2");
    CHECK(Cert::loadCert("cert2").get() == nullptr);
}

#endif


TEST_CASE("Cert request", "[Certs]") {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Retained<CertSigningRequest> csr = new CertSigningRequest(DistinguishedName(kSubjectName), key);
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
    Retained<Cert> caCert = new Cert(DistinguishedName(kCAName), caIssuerParams, caKey);
    cerr << "CA cert info:\n" << caCert->summary("\t");

    // Sign it:
    Cert::IssuerParameters caClientParams;
    caClientParams.validity_secs = 3600*24;
    Retained<Cert> clientCert = csr2->sign(caClientParams, caKey, caCert);

    cerr << "Client cert info:\n" << clientCert->summary("\t");

    CHECK(clientCert->isSigned());
    CHECK(!clientCert->isSelfSigned());
}


TEST_CASE("Cert concatenation", "[Certs]") {
    alloc_slice pem;
    {
        Retained<PrivateKey> key1, key2;
        Retained<Cert> cert1, cert2;
        tie(key1, cert1) = makeCert(kSubjectName);
        tie(key2, cert2) = makeCert(kSubject2Name);
        CHECK(!cert1->hasChain());
        CHECK(!cert1->next());
        CHECK(cert1->dataOfChain() == cert1->data(KeyFormat::PEM));

        cert1->append(cert2);

        cerr << string(cert1->summary()) << "\n";
        cerr << string(cert2->summary()) << "\n";

        CHECK(cert1->hasChain());
        CHECK(!cert2->hasChain());
        CHECK(!cert2->next());
        REQUIRE(cert1->next().get() == cert2);

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
        CHECK(cert1->next().get() == next);
        CHECK(cert1->dataOfChain() == pem);
        cerr << "Done\n";
    }

    // Reconstitute both certs from the saved PEM data:
    Retained<Cert> cert = new Cert(pem);
    CHECK(cert->hasChain());
    auto next = cert->next();
    REQUIRE(next);
    CHECK(!next->hasChain());
    CHECK(cert->next().get() == next);
    CHECK(cert->dataOfChain() == pem);
    CHECK(cert->subjectName() == kSubjectName);
    CHECK(next->subjectName() == kSubject2Name);
}

TEST_CASE("Cert request parsing", "[Certs]") {
    const char *kCSR = "-----BEGIN CERTIFICATE REQUEST-----\n"
                        "MIICzzCCAbcCAQAwNzEQMA4GA1UEAwwHUHVwc2hhdzESMBAGA1UECgwJQ291Y2hi\n"
                        "YXNlMQ8wDQYDVQQLDAZNb2JpbGUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n"
                        "AoIBAQC7hY5Q7zi49HKBL4iG0kmefWMvIEknUnCkour86HNxQhckePISyeHtXWgu\n"
                        "Mnugz7Y2qsO3Nje2c2PgNeFmxuDl0Zg5XdpWPe2OoKqABo8HqtICLDSlu61MkSdG\n"
                        "FXh4h2SLu0H5U7+Y40OnQB5VTSDDt9ThwVFJCCF/8o3THyIGZCphq0J00HllJpbI\n"
                        "AbrPBVd3ytvAixAYFyOCtkX/wpTykdNkv8D4DHg7x7Eu6+/lLkyi27m5ohJtkPbl\n"
                        "6YAXXuiQNs1MjWBke/dcOXRiqht/KOAZrox87kSz89LBoULqp1iyjsIpUn9MhHRi\n"
                        "8/R86OArHjwPppf66U2FLtP/j/DNAgMBAAGgUzBRBgkqhkiG9w0BCQ4xRDBCMCAG\n"
                        "A1UdEQQZMBeBFXB1cHNoYXdAY291Y2hiYXNlLm9yZzARBglghkgBhvhCAQEEBAMC\n"
                        "B4AwCwYDVR0PBAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQCSrGPATWk8eUT9lBUM\n"
                        "UXNchheMx4D+5SQDFKcy17njOVe+RKU2Y5iRMYxZ3MMzjj3YivLpVpVXBqR5HU52\n"
                        "pHytIUcs/jM5OlLWHLQ+5V++FkGl5f/KiLFFjf3kgvZctySt+cxiGQbCOd05C9RK\n"
                        "pyHsBaX9bToLflioCN2d9nRoXljtXwFh3507p970pQBXdBNdoLB55mg6VkLPO6gp\n"
                        "PR1Ks+RTqczX1a3Cst4dLP5E7RgY3Z0SiRQJeIv0plNc+Stebz8VZOYIBDA1Y0Dv\n"
                        "yKnZyB2LcxENgDD3fCw+4zjZWbuS0kHg6SXQ78IphnpB7gTCYG1QjNfKh/wNkvuQ\n"
                        "1ZF7\n"
                        "-----END CERTIFICATE REQUEST-----\n";

    Retained<CertSigningRequest> csr = new CertSigningRequest(slice(kCSR));
    CHECK(slice(csr->subjectName()) == "CN=Pupshaw, O=Couchbase, OU=Mobile"_sl);
    CHECK(csr->keyUsage() == 0x80);
    CHECK(csr->nsCertType() == 0x80);
    auto san = csr->subjectAltNames();
    REQUIRE(san.size() == 1);
    CHECK(san[0].first == SANTag::kRFC822Name);
    CHECK(san[0].second == "pupshaw@couchbase.org"_sl);
}

#if 0
TEST_CASE_METHOD(C4Test, "Send CSR to CA failure", "[Certs][.SyncServer]") {
    Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
    Retained<CertSigningRequest> csr = new CertSigningRequest(DistinguishedName(kSubjectName), key);

    // Bogus URL on local SG
    litecore::net::Address address(alloc_slice("https://localhost:4994/_certsign"_sl));

    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionPinnedServerCert));
    enc.writeData(readFile(sReplicatorFixturesDir + "cert.pem"));
    enc.endDict();
    AllocedDict options(enc.finish());

    atomic<bool> finished = false;
    Retained<Cert> cert;
    C4Error error;
    auto rq = retained(new litecore::REST::CertRequest);
    rq->start(csr, address, options, [&](Cert *cert_, C4Error error_) {
        cert = cert_;
        error = error_;
        finished = true;
    });

    REQUIRE_BEFORE(5s, finished);

    CHECK(cert == nullptr);
    CHECK(error.domain == WebSocketDomain);
    CHECK(error.code == 404);
}
#endif
