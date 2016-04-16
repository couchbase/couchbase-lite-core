//
// RetryHandler.cs
//
// Author:
//  Jim Borden  <jim.borden@couchbase.com>
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
using System.Collections.Generic;
using System.Threading;

namespace CBForest
{

    #region Delegates

    /// <summary>
    /// A delegate for calling native functions that return
    /// a bool and have an out error parameter
    /// </summary>
    public unsafe delegate bool C4TryLogicDelegate1(C4Error* err);

    /// <summary>
    /// A delegate for calling native functions that return
    /// a pointer and have an out error parameter
    /// </summary>
    public unsafe delegate void* C4TryLogicDelegate2(C4Error *err);

    /// <summary>
    /// A delegate for calling native functions that return
    /// an int and have an out error parameter
    /// </summary>
    public unsafe delegate int C4TryLogicDelegate3(C4Error *err);

    #endregion

    /// <summary>
    /// A rudimentary retry handler with options for allowing specific errors
    /// and custom exception handling
    /// </summary>
    public sealed class RetryHandler
	{

        #region Constants

        private const int RETRY_TIME = 200; // ms
        private const uint RETRY_ATTEMPTS = 5;

        #endregion

        #region Variables

        private Action<CBForestException> _exceptionHandler;
        private uint _maxAttempts;
        private List<C4Error> _allowedErrors = new List<C4Error>();

        #endregion

        #region Properties

        /// <summary>
        /// Gets the exception thrown during the operation, if any
        /// </summary>
        public CBForestException Exception { get; private set; }

        #endregion

        #region Public Methods

        /// <summary>
        /// The main entry point into this class.  It will retry the operation a specified
        /// number of times if the result of the operation is that the database is busy.
        /// </summary>
        /// <returns>A retry handler object for further fluent operations</returns>
        /// <param name="maxAttempts">The number of times to retry before giving up</param>
        public static RetryHandler RetryIfBusy(uint maxAttempts = RETRY_ATTEMPTS)
        {
            if (maxAttempts == 0) {
                throw new ArgumentException("Surely you want to try more than zero times", "maxAttempts");
            }

            var retVal = new RetryHandler();
            retVal._maxAttempts = maxAttempts;
            return retVal;
        }

        /// <summary>
        /// Allows the operation to succeed even if an error with the
        /// given code and domain occurs
        /// </summary>
        /// <returns>The current object for further fluent operations</returns>
        /// <param name="code">The code of the error to allow.</param>
        /// <param name="domain">The domain of the error to allow.</param>
        public RetryHandler AllowError(int code, C4ErrorDomain domain)
        {
            return AllowError(new C4Error { code = code, domain = domain });
        }

        /// <summary>
        /// Allows the operation to succeed even if the given error
        /// occurs
        /// </summary>
        /// <returns>The current object for further fluent operations</returns>
        /// <param name="error">The error to allow.</param>
        public RetryHandler AllowError(C4Error error)
        {
            _allowedErrors.Add(error);
            return this;
        }

        /// <summary>
        /// Allows the operation to succeed even if any of the
        /// given errors occur
        /// </summary>
        /// <returns>The current object for further fluent operations</returns>
        /// <param name="errors">The errors to allow.</param>
        public RetryHandler AllowErrors(params C4Error[] errors)
        {
            return AllowErrors((IEnumerable<C4Error>)errors);
        }

        /// <summary>
        /// Allows the operation to succeed even if any of the
        /// given errors occur
        /// </summary>
        /// <returns>The current object for further fluent operations</returns>
        /// <param name="errors">The errors to allow.</param>
        public RetryHandler AllowErrors(IEnumerable<C4Error> errors)
        {
            foreach (var error in errors) {
                AllowError(error);
            }

            return this;
        }

        /// <summary>
        /// Sets the handler for any exception generated during the operation
        /// that is not allowed by any of the AllowError API calls.  This will
        /// stop the retry handler from throwing.
        /// </summary>
        /// <returns>The current object for further fluent operations</returns>
        /// <param name="exceptionHandler">The logic for handling exceptions</param>
        public RetryHandler HandleExceptions(Action<CBForestException> exceptionHandler)
        {
            _exceptionHandler = exceptionHandler;
            return this;
        }

        /// <summary>
        /// Executes the specified operation
        /// </summary>
        /// <returns>The result of the operation</returns>
        /// <param name="block">The operation to run.</param>
        public unsafe bool Execute(C4TryLogicDelegate1 block)
        {
            return Execute(block, 0);
        }

        /// <summary>
        /// Executes the specified operation
        /// </summary>
        /// <returns>The result of the operation</returns>
        /// <param name="block">The operation to run.</param>
        public unsafe void* Execute(C4TryLogicDelegate2 block)
        {
            return Execute(block, 0);
        }

        /// <summary>
        /// Executes the specified operation
        /// </summary>
        /// <returns>The result of the operation</returns>
        /// <param name="block">The operation to run.</param>
        public unsafe int Execute(C4TryLogicDelegate3 block)
        {
            return Execute(block, 0);
        }

        #endregion

        #region Private Methods

        private unsafe bool Execute(C4TryLogicDelegate1 block, int attemptCount)
        {
            if (attemptCount > _maxAttempts) {
                ThrowOrHandle();
            }

            var err = default(C4Error);
            if (block(&err)) {
                Exception = null;
                return true;
            }

            Exception = new CBForestException(err);
            if (err.domain == C4ErrorDomain.ForestDB && err.code == (int)ForestDBStatus.HandleBusy) {
                Thread.Sleep(RETRY_TIME);
                return Execute(block, attemptCount + 1);
            }

            ThrowOrHandle();
            return false;
        }

        private unsafe void* Execute(C4TryLogicDelegate2 block, int attemptCount)
        {
            if (attemptCount > _maxAttempts) {
                ThrowOrHandle();
            }

            var err = default(C4Error);
            var retVal = block(&err);
            if (retVal != null) {
                Exception = null;
                return retVal;
            }

            Exception = new CBForestException(err);
            if (err.domain == C4ErrorDomain.ForestDB && err.code == (int)ForestDBStatus.HandleBusy) {
                Thread.Sleep(RETRY_TIME);
                return Execute(block, attemptCount + 1);
            }

            ThrowOrHandle();
            return retVal;
        }

        private unsafe int Execute(C4TryLogicDelegate3 block, int attemptCount)
        {
            if (attemptCount > _maxAttempts) {
                ThrowOrHandle();
            }

            var err = default(C4Error);
            var retVal = block(&err);
            if (retVal >= 0) {
                Exception = null;
                return retVal;
            }

            Exception = new CBForestException(err);
            if (err.domain == C4ErrorDomain.ForestDB && err.code == (int)ForestDBStatus.HandleBusy) {
                Thread.Sleep(RETRY_TIME);
                return Execute(block, attemptCount + 1);
            }

            ThrowOrHandle();
            return retVal;
        }

        private void ThrowOrHandle()
        {
            foreach (var error in _allowedErrors) {
                if(error.Equals(Exception.Error) || (error.domain == C4ErrorDomain.Any &&
                    error.code.Equals(Exception.Error.code))) {
                    return;
                }
            }

            if (_exceptionHandler != null) {
                _exceptionHandler(Exception);
                return;
            }

            throw Exception;
        }

        #endregion
	}
}

