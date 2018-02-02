//
// SQLiteTempDirectory.h
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

// This can't go inside LiteCoreStatic because it needs to be compiled with /ZW (consume
// Windows Runtime extensions) and any library that uses that cannot contain C source

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
extern "C" void setSqliteTempDirectory();
#endif
