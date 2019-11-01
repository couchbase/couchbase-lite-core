//
// DatabaseTest.cc
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

#include "Database.hh"
#include "FleeceImpl.hh"
#include "JSONEncoder.hh"
#include "JSON5.hh"
#include "crc32c.h"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace fleece::impl;
using namespace std;

namespace {
extern string kJSONDoc;


TEST_CASE("Document body CRC32") {
    Retained<SharedKeys> sharedKeys = new SharedKeys();
    alloc_slice body = JSONConverter::convertJSON(slice(ConvertJSON5(kJSONDoc)), sharedKeys);
    uint32_t initialCRC32 = 123456789;

    // Let LiteCore compute a CRC32 of the canonical JSON body:
    int actualCRC = TreeDocumentFactory::digestDocumentBody(body, sharedKeys, initialCRC32);

    // Do it the more expensive way by actually generating the canonical JSON first:
    // (compare with the implementation of TreeDocumentFactory::digestDocumentBody)
    Scope scope(body, sharedKeys);
    const Value *root = Value::fromTrustedData(body);
    JSONEncoder enc;
    enc.setCanonical(true);
    enc.writeValue(root);
    alloc_slice canonicalJson = enc.finish();
    int expectedCRC = crc32c((const uint8_t*)canonicalJson.buf, canonicalJson.size, initialCRC32);

    CHECK(actualCRC == expectedCRC);
}


string kJSONDoc =
"{"
"  '_id': '56516c81b864942e1acca6d9',"
"  'type': 'person',"
"  'index': 0,"
"  'guid': 'c2b61d0d-ac83-47f6-ae59-b6a8e3bf3ab8',"
"  'isActive': true,"
"  'balance': '$1,458.82',"
"  'picture': 'http://placehold.it/32x32',"
"  'age': 30,"
"  'eyeColor': 'blue',"
"  'name': 'Glenda Morse',"
"  'gender': 'female',"
"  'company': 'BLEEKO',"
"  'email': 'glendamorse@bleeko.com',"
"  'phone': '+1 (911) 413-2443',"
"  'address': '927 Hinsdale Street, Virgie, Ohio, 4436',"
"  'about': 'Elit ut duis deserunt excepteur id in tempor ipsum sunt. Pariatur ullamco ullamco aliqua dolore aliqua do ea mollit est aute dolore. Amet qui velit sit aliquip ipsum deserunt veniam cupidatat voluptate nisi elit. Est dolor enim eiusmod amet tempor culpa commodo dolor. Nostrud aute deserunt do qui dolor. Ad exercitation id sit anim deserunt eiusmod elit.\\r\\n',"
"  'registered': '2014-01-28T05:37:03 +08:00',"
"  'latitude': 40.941286,"
"  'longitude': -21.152958,"
"  'tags': ["
"    'quis',"
"    'laborum',"
"    'officia',"
"    'adipisicing',"
"    'et',"
"    'laborum',"
"    'tempor'"
"  ],"
"  'friends': ["
"    {"
"      'id': 0,"
"      'name': 'Magdalena Moore'"
"    },"
"    {"
"      'id': 1,"
"      'name': 'Watts Townsend'"
"    },"
"    {"
"      'id': 2,"
"      'name': 'Owens Everett'"
"    }"
"  ]"
"}";

}
