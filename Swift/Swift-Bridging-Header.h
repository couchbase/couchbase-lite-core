//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#include <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>

#define APPLE 1

FOUNDATION_EXPORT double SwiftForestVersionNumber;
FOUNDATION_EXPORT const unsigned char SwiftForestVersionString[];

#include "c4.h"
#include "c4Database.h"
#include "c4Document.h"
#include "c4View.h"
