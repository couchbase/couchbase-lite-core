//
// CBForestException.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

namespace CBForest
{
    /// <summary>
    /// An exception representing an error status from the native
    /// CBForest module
    /// </summary>
    public sealed class CBForestException : ApplicationException
    {

        #region Variables

        /// <summary>
        /// The error that caused the exception
        /// </summary>
        public readonly C4Error Error;

        #endregion

        #region Properties

        /// <summary>
        /// Gets the error code received from CBForest
        /// </summary>
        public int Code 
        {
            get {
                return Error.code;
            }
        }

        /// <summary>
        /// Gets the domain of the error code received from CBForest
        /// </summary>
        /// <value>The domain.</value>
        public C4ErrorDomain Domain
        {
            get {
                return Error.domain;
            }
        }

        #endregion

        #region Constructors

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="code">The code of the error that is the source of the exception.</param>
        /// <param name="domain">The domain of the error that is the source of the exception.</param>
        public CBForestException(int code, C4ErrorDomain domain) 
            : this(new C4Error { code = code, domain = domain })
        {
        }

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="error">The error that is the source of the exception.</param>
        public CBForestException(C4Error error)
            : base(String.Format("{0} (Code: {1})", Native.c4error_getMessage(error), error))
        {
            Error = error;
        }

        #endregion

    }
}

