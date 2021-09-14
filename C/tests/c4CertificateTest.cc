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

#include "c4Test.hh"

#ifdef COUCHBASE_ENTERPRISE

#include "c4Certificate.h"
#include "CertHelper.hh"

using namespace std;


TEST_CASE("C4Certificate smoke test", "[Certs][C]") {
    // Just make sure c4cert functions are exported from LiteCore and minimally functional...
    CertHelper certs;

    C4CertNameInfo name;
    C4Log("Client cert:");
    for (int i = 0; c4cert_subjectNameAtIndex(certs.temporaryClientIdentity.cert, i, &name); ++i) {
        C4Log("  %.*s = '%.*s'", SPLAT(name.id), SPLAT(name.value));
        if (i == 0) {
            CHECK(name.id == kC4Cert_CommonName);
            CHECK(name.value == "LiteCore Client Test"_sl);
        }
        c4slice_free(name.id);
        c4slice_free(name.value);
    }
}


#endif
