//
// c4.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#ifndef __cplusplus
#error "This is C++ only"
#endif

// ************************************************************************
// This is the "umbrella header" that includes the entire LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************

#include "c4BlobStore.hh"
#include "c4Certificate.hh"
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "c4Listener.hh"
#include "c4Observer.hh"
#include "c4Query.hh"
#include "c4Replicator.hh"
#include "c4Socket.hh"
