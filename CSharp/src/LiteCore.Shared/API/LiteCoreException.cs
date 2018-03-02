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
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;

using JetBrains.Annotations;

using LiteCore.Interop;

namespace Couchbase.Lite
{
    public enum CouchbaseLiteError
    {
        /// <summary>
        /// Internal assertion failure
        /// </summary>
        AssertionFailed = 1,    

        /// <summary>
        /// An unimplemented API call
        /// </summary>
        Unimplemented,

        /// <summary>
        /// Unsupported encryption algorithm
        /// </summary>
        UnsupportedEncryption = 4,

        /// <summary>
        /// Function must be called within a transaction
        /// </summary>
        NoTransaction,

        /// <summary>
        /// Revision contains corrupted/unreadable data
        /// </summary>
        CorruptRevisionData = 8,

        /// <summary>
        /// Database/KeyStore is not open
        /// </summary>
        NotOpen = 11,

        /// <summary>
        /// Document not found
        /// </summary>
        NotFound,

        /// <summary>
        /// Document update conflict
        /// </summary>
        Conflict = 14,

        /// <summary>
        /// Invalid function parameter or struct value
        /// </summary>
        InvalidParameter,

        /// <summary>
        /// Internal unexpected C++ exception
        /// </summary>
        UnexpectedError = 17,

        /// <summary>
        /// Database file can't be opened; may not exist
        /// </summary>
        CantOpenFile,

        /// <summary>
        /// File I/O error
        /// </summary>
        IOError,

        /// <summary>
        /// Memory allocation failed (out of memory?)
        /// </summary>
        MemoryError = 21,

        /// <summary>
        /// File is not writeable
        /// </summary>
        NotWriteable,

        /// <summary>
        /// Data is corrupted
        /// </summary>
        CorruptData,

        /// <summary>
        /// Database is busy / locked
        /// </summary>
        Busy,

        /// <summary>
        /// Function cannot be called while in a transaction
        /// </summary>
        NotInTransaction,

        /// <summary>
        /// Database can't be closed while a transaction is open
        /// </summary>
        TransactionNotClosed,

        ///// <summary>
        ///// (Unused)
        ///// </summary>
        //IndexBusy,

        /// <summary>
        /// Operation not supported on this database
        /// </summary>
        Unsupported = 28,

        /// <summary>
        /// File is not a database or encryption key is wrong
        /// </summary>
        UnreadableDatabase,

        /// <summary>
        /// Database exists but not in the format/storage requested
        /// </summary>
        WrongFormat,

        /// <summary>
        /// Encryption / Decryption error
        /// </summary>
        Crypto,

        /// <summary>
        /// Invalid query
        /// </summary>
        InvalidQuery,

        /// <summary>
        /// No such index, or query requires a nonexistent index
        /// </summary>
        MissingIndex,

        /// <summary>
        /// Unknown query param name, or param number out of range
        /// </summary>
        InvalidQueryParam,

        /// <summary>
        /// Unknown error from remote server
        /// </summary>
        RemoteError,

        /// <summary>
        /// Database file format is older than what I can open
        /// </summary>
        DatabaseTooOld,

        /// <summary>
        /// Database file format is newer than what I can open
        /// </summary>
        DatabaseTooNew,

        /// <summary>
        /// Invalid document ID
        /// </summary>
        BadDocID,

        /// <summary>
        /// Database can't be upgraded (might be unsupported dev version)
        /// </summary>
        CantUpgradeDatabase,

        /// <summary>
        /// Not an actual error, but serves as the lower bound for network related
        /// errors
        /// </summary>
        NetworkBase = 5000,

        /// <summary>
        /// DNS Lookup failed
        /// </summary>
        DNSFailure,

        /// <summary>
        /// DNS server doesn't know the hostname
        /// </summary>
        UnknownHost,

        /// <summary>
        /// Socket timeout during an operation
        /// </summary>
        Timeout,

        /// <summary>
        /// The provided URL is not valid
        /// </summary>
        InvalidUrl,

        /// <summary>
        /// Too many HTTP redirects for the HTTP client to handle
        /// </summary>
        TooManyRedirects,

        /// <summary>
        /// Failure during TLS handshake process
        /// </summary>
        TLSHandshakeFailed,

        /// <summary>
        /// The provided TLS certificate has expired
        /// </summary>
        TLSCertExpired,

        /// <summary>
        /// Cert isn't trusted for other reason
        /// </summary>
        TLSCertUntrusted,

        /// <summary>
        /// A required client certificate was not provided
        /// </summary>
        TLSClientCertRequired,

        /// <summary>
        /// Client certificate was rejected by the server
        /// </summary>
        TLSClientCertRejected,

        /// <summary>
        /// Self-signed cert, or unknow anchor cert
        /// </summary>
        TLSCertUnknownRoot,

        /// <summary>
        /// The client was redirected to an invalid location by the server
        /// </summary>
        InvalidRedirect,

        /// <summary>
        /// Not an actual error, but serves as the lower bound for HTTP related
        /// errors
        /// </summary>
        HTTPBase = 10000,

        /// <summary>
        /// Missing or incorrect user authentication
        /// </summary>
        HTTPAuthRequired = 10401,

        /// <summary>
        /// User doesn't have permission to access resource
        /// </summary>
        HTTPForbidden = 10403,

        /// <summary>
        /// Resource not found
        /// </summary>
        HTTPNotFound = 10404,

        /// <summary>
        /// HTTP proxy requires authentication
        /// </summary>
        HTTPProxyAuthRequired = 10407,

        /// <summary>
        /// Update conflict
        /// </summary>
        HTTPConflict = 10409,

        /// <summary>
        /// Data is too large to upload
        /// </summary>
        HTTPEntityTooLarge = 10413,

        /// <summary>
        /// Something's wrong with the server
        /// </summary>
        HTTPInternalServerError = 10500,

        /// <summary>
        /// Unimplemented server functionality
        /// </summary>
        HTTPNotImplemented = 10501,

        /// <summary>
        /// Service is down temporarily
        /// </summary>
        HTTPServiceUnavailable = 10503,

        /// <summary>
        /// Not an actual error, but serves as the lower bound for WebSocket
        /// related errors
        /// </summary>
        WebSocketBase = 11000,

        /// <summary>
        /// Peer has to close, e.g. because host app is quitting
        /// </summary>
        WebSocketGoingAway = 11001,

        /// <summary>
        /// Protocol violation: invalid framing data
        /// </summary>
        WebSocketProtocolError = 11002,

        /// <summary>
        /// Message payload cannot be handled
        /// </summary>
        WebSocketDataError = 11003,

        /// <summary>
        /// TCP socket closed unexpectedly
        /// </summary>
        WebSocketAbnormalClose = 11006,

        /// <summary>
        /// Unparseable WebSocket message
        /// </summary>
        WebSocketBadMessageFormat = 11007,

        /// <summary>
        /// Message violated unspecified policy
        /// </summary>
        WebSocketPolicyError = 11008,

        /// <summary>
        /// Message is too large for peer to handle
        /// </summary>
        WebSocketMessageTooBig = 11009,

        /// <summary>
        /// Peer doesn't provide a necessary extension
        /// </summary>
        WebSocketMissingExtension = 11010,

        /// <summary>
        /// Can't fulfill request due to "unexpected condition"
        /// </summary>
        WebSocketCantFulfill = 11011
    }

    public enum CouchbaseLiteErrorType
    {
        CouchbaseLite,
        POSIX,
        SQLite,
        Fleece
    }

    /// <summary>
    /// An exception representing one of the types of exceptions that can occur
    /// during Couchbase use
    /// </summary>
    public abstract class CouchbaseException : Exception
    {
        #region Constants

        private static readonly IReadOnlyList<int> _BugReportErrors = new List<int>
        {
            (int)C4ErrorCode.AssertionFailed,
            (int)C4ErrorCode.Unimplemented,
            (int)C4ErrorCode.Unsupported,
            (int)C4ErrorCode.UnsupportedEncryption,
            (int)C4ErrorCode.NoTransaction,
            (int)C4ErrorCode.BadRevisionID,
            (int)C4ErrorCode.UnexpectedError
        };

        #endregion

        #region Variables

        private delegate string ErrorMessageVisitor(C4Error err);

        #endregion

        #region Properties

        public CouchbaseLiteErrorType Domain { get; }

        public int Error { get; }

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
         C4Error LiteCoreError { get; }

        #endregion

        #region Constructors

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
            CouchbaseException(C4Error err) : this(err, GetMessage(err))
        {
           
        }

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
            CouchbaseException(C4Error err, string message) : base(message)
        {
            LiteCoreError = err;
            Error = MapError(err);
            Domain = MapDomain(err);
        }

        #endregion

        [NotNull]
        internal static CouchbaseException Create(C4Error err)
        {
            switch (err.domain) {
                case C4ErrorDomain.FleeceDomain:
                    return new CouchbaseFleeceException(err);
                case C4ErrorDomain.LiteCoreDomain:
                    return new CouchbaseLiteException(err);
                case C4ErrorDomain.NetworkDomain:
                    return new CouchbaseNetworkException(err);
                case C4ErrorDomain.POSIXDomain:
                    return new CouchbasePosixException(err);
                case C4ErrorDomain.SQLiteDomain:
                    return new CouchbaseSQLiteException(err);
                case C4ErrorDomain.WebSocketDomain:
                    return new CouchbaseWebsocketException(err);
                default:
                    return new CouchbaseLiteException(C4ErrorCode.UnexpectedError);
            }
        }

        #region Private Methods

        private static string GetMessage(C4Error err)
        {
            foreach (var visitor in MessageVisitors()) {
                var msg = visitor(err);
                if (msg != null) {
                    return msg;
                }
            }

            Debug.Assert(false, "Panic!  No suitable error message found");
            return null;
        }

        private static CouchbaseLiteErrorType MapDomain(C4Error err)
        {
            switch (err.domain) {
                case C4ErrorDomain.FleeceDomain:
                    return CouchbaseLiteErrorType.Fleece;
                case C4ErrorDomain.POSIXDomain:
                    return CouchbaseLiteErrorType.POSIX;
                case C4ErrorDomain.SQLiteDomain:
                    return CouchbaseLiteErrorType.SQLite;
                default:
                    return CouchbaseLiteErrorType.CouchbaseLite;
            }
        }

        private static int MapError(C4Error err)
        {
            switch (err.domain) {
                case C4ErrorDomain.NetworkDomain:
                    return err.code + (int) CouchbaseLiteError.NetworkBase;
                case C4ErrorDomain.WebSocketDomain:
                    return err.code + (int) CouchbaseLiteError.WebSocketBase;
                default:
                    return err.code;
            }
        }

        [NotNull]
        [ItemNotNull]
        private static IEnumerable<ErrorMessageVisitor> MessageVisitors()
        {
            yield return VisitBugReportList;
            yield return VisitCantUpgrade;
            yield return VisitDefault;
        }

        private static string VisitBugReportList(C4Error err)
        {
            if (err.domain == C4ErrorDomain.LiteCoreDomain && _BugReportErrors.Contains(err.code)) {
                return
                    $"CouchbaseLiteException ({err.domain} / {err.code}): {Native.c4error_getMessage(err)}.  Please file a bug report at https://github.com/couchbase/couchbase-lite-net/";
            }

            return null;
        }

        private static string VisitCantUpgrade(C4Error err)
        {
            if (err.domain == C4ErrorDomain.LiteCoreDomain && err.code == (int) C4ErrorCode.CantUpgradeDatabase) {
                return
                    $"CouchbaseLiteException ({err.domain} / {err.code}): {Native.c4error_getMessage(err)}.  If the previous database version was a version produced by a production version of Couchbase Lite, then please file a bug report at https://github.com/couchbase/couchbase-lite-net/";
            }

            return null;
        }

        private static string VisitDefault(C4Error err) => $"CouchbaseLiteException ({err.domain} / {err.code}): {Native.c4error_getMessage(err)}.";

        #endregion
    }

    public sealed class CouchbaseLiteException : CouchbaseException
    {
        #region Properties

        public new CouchbaseLiteError Error => (CouchbaseLiteError) base.Error;

        #endregion

        #region Constructors

        internal CouchbaseLiteException(C4Error err) : base(err)
        {

        }

        internal CouchbaseLiteException(C4ErrorCode errCode) : base(new C4Error(errCode))
        {

        }

        internal CouchbaseLiteException(C4ErrorCode errCode, string message) : base(new C4Error(errCode), message)
        {

        }

        #endregion
    }

    public sealed class CouchbaseFleeceException : CouchbaseException
    {
        #region Constructors

        internal CouchbaseFleeceException(C4Error err) : base(err)
        {

        }

        internal CouchbaseFleeceException(FLError errCode) : base(new C4Error(errCode))
        {

        }

        internal CouchbaseFleeceException(FLError errCode, string message) : base(new C4Error(errCode), message)
        {

        }

        #endregion
    }

    public sealed class CouchbaseSQLiteException : CouchbaseException
    {
        #region Properties

        public SQLiteStatus BaseError => (SQLiteStatus) (Error & 0xFF);

        #endregion

        #region Constructors

        internal CouchbaseSQLiteException(C4Error err) : base(err)
        {

        }

        internal CouchbaseSQLiteException(int errCode) : base(new C4Error(C4ErrorDomain.SQLiteDomain, errCode))
        {

        }

        internal CouchbaseSQLiteException(int errCode, string message) : base(new C4Error(C4ErrorDomain.SQLiteDomain, errCode), message)
        {

        }

        #endregion
    }

    public sealed class CouchbaseWebsocketException : CouchbaseException
    {
        #region Properties

        public new CouchbaseLiteError Error => (CouchbaseLiteError) base.Error;

        #endregion

        #region Constructors

        internal CouchbaseWebsocketException(C4Error err) : base(err)
        {

        }

        internal CouchbaseWebsocketException(int errCode) : base(new C4Error(C4ErrorDomain.WebSocketDomain, errCode))
        {

        }

        internal CouchbaseWebsocketException(int errCode, string message) : base(new C4Error(C4ErrorDomain.WebSocketDomain, errCode), message)
        {

        }

        #endregion
    }

    public sealed class CouchbaseNetworkException : CouchbaseException
    {
        #region Properties

        public new CouchbaseLiteError Error => (CouchbaseLiteError) base.Error;

        #endregion

        #region Constructors

        internal CouchbaseNetworkException(C4Error err) : base(err)
        {

        }

        internal CouchbaseNetworkException(C4NetworkErrorCode errCode) : base(new C4Error(errCode))
        {

        }

        internal CouchbaseNetworkException(C4NetworkErrorCode errCode, string message) : base(new C4Error(errCode), message)
        {

        }

        #endregion
    }

    public sealed class CouchbasePosixException : CouchbaseException
    {
        #region Properties

        public new PosixStatus Error => (PosixStatus) base.Error;

        #endregion

        #region Constructors

        internal CouchbasePosixException(C4Error err) : base(err)
        {

        }

        internal CouchbasePosixException(int errCode) : base(new C4Error(C4ErrorDomain.POSIXDomain, errCode))
        {

        }

        internal CouchbasePosixException(int errCode, string message) : base(new C4Error(C4ErrorDomain.POSIXDomain, errCode), message)
        {

        }

        #endregion
    }
}
