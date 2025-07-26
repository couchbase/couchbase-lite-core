//
// c4CertificateTest.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef COUCHBASE_ENTERPRISE

#    include "c4Certificate.h"
#    include "CertHelper.hh"
#ifdef WIN32
#    include "Winsock2.h"
#else
#    include <arpa/inet.h>
#endif

using namespace std;

TEST_CASE("C4Certificate smoke test", "[Certs][C]") {
    // Just make sure c4cert functions are exported from LiteCore and minimally functional...
    CertHelper certs;

    C4CertNameInfo name;
    C4Log("Client cert:");
    for ( int i = 0; c4cert_subjectNameAtIndex(certs.temporaryClientIdentity.cert, i, &name); ++i ) {
        C4Log("  %.*s = '%.*s'", SPLAT(name.id), SPLAT(name.value));
        if ( i == 0 ) {
            CHECK(name.id == kC4Cert_CommonName);
            CHECK(name.value == "LiteCore Client Test"_sl);
        }
        c4slice_free(name.id);
        c4slice_free(name.value);
    }
}

TEST_CASE("C4Certificate Subject Name", "[Certs][C]") {
    auto ipAddr = htonl(inet_addr("127.0.0.1"));
    fleece::slice ipAddrSlice((void *)&ipAddr, sizeof(in_addr_t));
    C4CertNameComponent nameComponents[] = {{kC4Cert_CommonName, "CommonName"_sl},
                                            {kC4Cert_Pseudonym, "Pseudonym"_sl},
                                            {kC4Cert_GivenName, "GivenName"_sl},
                                            {kC4Cert_Surname, "Surname"_sl},
                                            {kC4Cert_Organization, "Organiztion"_sl},
                                            {kC4Cert_OrganizationUnit, "OrganizationUnit"_sl},
                                            {kC4Cert_PostalAddress, "PostalAddress"_sl},
                                            {kC4Cert_Locality, "Locality"_sl},
                                            {kC4Cert_PostalCode, "PostalCode"_sl},
                                            {kC4Cert_StateOrProvince, "StateOrProvince"_sl},
                                            {kC4Cert_Country, "Country"_sl},
                                            {kC4Cert_EmailAddress, "EmailAddress"_sl},
                                            {kC4Cert_Hostname, "Hostname"_sl},
                                            {kC4Cert_URL, "URL"_sl},
                                            {kC4Cert_IPAddress, ipAddrSlice},
                                            {kC4Cert_RegisteredID, "RegisteredID"_sl}};
    size_t              compCount        = std::size(nameComponents);

    c4::ref<C4Cert> cert = [&]() -> C4Cert* {
        c4::ref<C4KeyPair> key = c4keypair_generate(kC4RSA, 2048, false, nullptr);
        REQUIRE(key);

        c4::ref<C4Cert> csr = c4cert_createRequest(nameComponents, compCount, kC4CertUsage_TLSClient, key, nullptr);
        REQUIRE(csr);

        C4CertIssuerParameters issuerParams = kDefaultCertIssuerParameters;
        issuerParams.validityInSeconds      = 3600;
        issuerParams.isCA                   = false;
        return c4cert_signRequest(csr, &issuerParams, key, nullptr, nullptr);
    }();
    REQUIRE(cert);

    for ( size_t i = 0; i < compCount; ++i ) {
        alloc_slice nameComp = c4cert_subjectNameComponent(cert, nameComponents[i].attributeID);
        CHECK(nameComp == nameComponents[i].value);
    }
}

#endif  //#ifdef COUCHBASE_ENTERPRISE
