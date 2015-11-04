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
using System.Linq;
using System.Threading;


namespace CBForest
{
    public unsafe delegate bool C4TryLogicDelegate1(C4Error* err);

    public unsafe delegate void* C4TryLogicDelegate2(C4Error *err);

    public unsafe delegate int C4TryLogicDelegate3(C4Error *err);

    public sealed class RetryHandler
	{
        private const int RETRY_TIME = 200; // ms
        private const uint RETRY_ATTEMPTS = 5;

        private Action<CBForestException> _exceptionHandler;
        private uint _maxAttempts;
        private List<C4Error> _allowedErrors = new List<C4Error>();

        public CBForestException Exception { get; private set; }

        public static RetryHandler RetryIfBusy(uint maxAttempts = RETRY_ATTEMPTS)
        {
            if (maxAttempts == 0) {
                throw new ArgumentException("Surely you want to try more than zero times", "maxAttempts");
            }

            var retVal = new RetryHandler();
            retVal._maxAttempts = maxAttempts;
            return retVal;
        }

        public RetryHandler AllowError(int code, C4ErrorDomain domain)
        {
            return AllowError(new C4Error { code = code, domain = domain });
        }

        public RetryHandler AllowError(C4Error error)
        {
            _allowedErrors.Add(error);
            return this;
        }

        public RetryHandler AllowErrors(params C4Error[] errors)
        {
            return AllowErrors((IEnumerable<C4Error>)errors);
        }

        public RetryHandler AllowErrors(IEnumerable<C4Error> errors)
        {
            foreach (var error in errors) {
                AllowError(error);
            }

            return this;
        }

        public RetryHandler HandleExceptions(Action<CBForestException> exceptionHandler)
        {
            _exceptionHandler = exceptionHandler;
            return this;
        }

        public unsafe bool Execute(C4TryLogicDelegate1 block)
        {
            return Execute(block, 0);
        }

        public unsafe void* Execute(C4TryLogicDelegate2 block)
        {
            return Execute(block, 0);
        }

        public unsafe int Execute(C4TryLogicDelegate3 block)
        {
            return Execute(block, 0);
        }

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
            if (_allowedErrors.Contains(Exception.Error)) {
                return;
            }

            if (_exceptionHandler != null) {
                _exceptionHandler(Exception);
                return;
            }

            throw Exception;
        }
	}
}

