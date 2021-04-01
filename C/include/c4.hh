//
// c4.hh
//
// Copyright (c) 2021 Couchbase, Inc All rights reserved.
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

#pragma once

#ifndef __cplusplus
#error "This is C++ only"
#endif

#include "c4BlobStore.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "c4Observer.hh"
#include "c4Query.hh"
#include "c4Replicator.hh"

#include "c4Socket.h"

#ifdef COUCHBASE_ENTERPRISE
#include "c4Certificate.hh"
#endif
