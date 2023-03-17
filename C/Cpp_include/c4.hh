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
#    error "This is C++ only"
#endif

// ************************************************************************
// This is the "umbrella header" that includes the entire LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************

// These headers are marked as keep so Clangd knows they aren't intended to be used directly in this file

#include "c4BlobStore.hh"      // IWYU pragma: keep
#include "c4Certificate.hh"    // IWYU pragma: keep
#include "c4Collection.hh"     // IWYU pragma: keep
#include "c4Database.hh"       // IWYU pragma: keep
#include "c4Document.hh"       // IWYU pragma: keep
#include "c4DocEnumerator.hh"  // IWYU pragma: keep
#include "c4Listener.hh"       // IWYU pragma: keep
#include "c4Observer.hh"       // IWYU pragma: keep
#include "c4Query.hh"          // IWYU pragma: keep
#include "c4Replicator.hh"     // IWYU pragma: keep
#include "c4Socket.hh"         // IWYU pragma: keep
