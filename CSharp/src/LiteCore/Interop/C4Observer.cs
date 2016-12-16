//
// C4Observer.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

using System.Runtime.InteropServices;

using C4SequenceNumber = System.UInt64;

namespace LiteCore.Interop
{
    public struct C4DatabaseObserver
    {

    }

    public struct C4DocumentObserver
    {

    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void C4DatabaseObserverCallback(C4DatabaseObserver* observer, void* context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void C4DocumentObserverCallback(C4DocumentObserver* observer, C4Slice docID, C4SequenceNumber sequence, void* context);
}