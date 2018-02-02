//
// LiteCoreException.cs
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
using System;

using LiteCore.Interop;

namespace LiteCore
{
    /// <summary>
    /// An exception representing an error coming from the native LiteCore library
    /// </summary>
    public sealed class LiteCoreException : Exception
    {
        #region Properties

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
         C4Error Error { get; }

        #endregion

        #region Constructors

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
         LiteCoreException(C4Error err) : base($"LiteCoreException ({err.domain} / {err.code}): {Native.c4error_getMessage(err)}")
        {
            Error = err;
        }

        #endregion
    }
}
