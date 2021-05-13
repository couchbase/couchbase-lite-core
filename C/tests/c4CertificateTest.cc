//
// c4CertificateTest.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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
