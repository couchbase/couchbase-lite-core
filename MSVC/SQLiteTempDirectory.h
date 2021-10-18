//
// SQLiteTempDirectory.h
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// This can't go inside LiteCoreStatic because it needs to be compiled with /ZW (consume
// Windows Runtime extensions) and any library that uses that cannot contain C source

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
extern "C" void setSqliteTempDirectory();
#endif
