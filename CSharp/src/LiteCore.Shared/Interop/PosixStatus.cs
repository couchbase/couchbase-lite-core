// 
//  PosixStatus.cs
// 
//  Copyright (c) 2016 Couchbase, Inc All rights reserved.
// 
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
// 
//  http://www.apache.org/licenses/LICENSE-2.0
// 
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// 
namespace Couchbase.Lite
{
    public enum PosixStatus
    {
        PERM = 1,       /* Operation not permitted */
        NOENT = 2,       /* No such file or directory */
        SRCH = 3,       /* No such process */
        INTR = 4,       /* Interrupted system call */
        IO = 5,       /* Input/output error */
        NXIO = 6,       /* Device not configured */
        TOOBIG = 7,       /* Argument list too long */
        NOEXEC = 8,       /* Exec format error */
        BADF = 9,       /* Bad file descriptor */
        CHILD = 10,      /* No child processes */
        DEADLK = 11,      /* Resource deadlock avoided */
        NOMEM = 12,      /* Cannot allocate memory */
        ACCES = 13,      /* Permission denied */
        FAULT = 14,      /* Bad address */
        NOTBLK = 15,      /* Block device required */
        BUSY = 16,      /* Device / Resource busy */
        EXIST = 17,      /* File exists */
        XDEV = 18,        /* Cross-device link */
        NODEV = 19,      /* Operation not supported by device */
        NOTDIR = 20,      /* Not a directory */
        ISDIR = 21,      /* Is a directory */
        INVAL = 22,      /* Invalid argument */
        NFILE = 23,      /* Too many open files in system */
        MFILE = 24,      /* Too many open files */
        NOTTY = 25,      /* Inappropriate ioctl for device */
        TXTBSY = 26,      /* Text file busy */
        FBIG = 27,      /* File too large */
        NOSPC = 28,      /* No space left on device */
        SPIPE = 29,      /* Illegal seek */
        ROFS = 30,      /* Read-only file system */
        MLINK = 31,      /* Too many links */
        PIPE = 32,      /* Broken pipe */

        /* math software */
        DOM = 33,      /* Numerical argument out of domain */
        RANGE = 34,      /* Result too large */

        /* non-blocking and interrupt i/o */
        AGAIN = 35,                 /* Resource temporarily unavailable */
        EWOULDBLOCK = AGAIN,      /* Operation would block */
        INPROGRESS = 36,            /* Operation now in progress */
        ALREADY = 37,      /* Operation already in progress */

        /* ipc/network software -- argument errors */
        NOTSOCK = 38,      /* Socket operation on non-socket */
        DESTADDRREQ = 39,      /* Destination address required */
        MSGSIZE = 40,      /* Message too long */
        PROTOTYPE = 41,      /* Protocol wrong type for socket */
        NOPROTOOPT = 42,      /* Protocol not available */
        PROTONOSUPPORT = 43,      /* Protocol not supported */
        SOCKTNOSUPPORT = 44,      /* Socket type not supported */
        NOTSUP = 45,      /* Operation not supported */
        EOPNOTSUPP = NOTSUP,    /* Operation not supported on socket */
        PFNOSUPPORT = 46,      /* Protocol family not supported */
        AFNOSUPPORT = 47,      /* Address family not supported by protocol family */
        ADDRINUSE = 48,      /* Address already in use */
        ADDRNOTAVAIL = 49,      /* Can't assign requested address */

        /* ipc/network software -- operational errors */
        NETDOWN = 50,      /* Network is down */
        NETUNREACH = 51,      /* Network is unreachable */
        NETRESET = 52,      /* Network dropped connection on reset */
        CONNABORTED = 53,      /* Software caused connection abort */
        CONNRESET = 54,      /* Connection reset by peer */
        NOBUFS = 55,      /* No buffer space available */
        ISCONN = 56,      /* Socket is already connected */
        NOTCONN = 57,      /* Socket is not connected */
        SHUTDOWN = 58,      /* Can't send after socket shutdown */
        TOOMANYREFS = 59,      /* Too many references: can't splice */
        TIMEDOUT = 60,      /* Operation timed out */
        CONNREFUSED = 61,      /* Connection refused */
        LOOP = 62,      /* Too many levels of symbolic links */
        NAMETOOLONG = 63,      /* File name too long */

        /* should be rearranged */

        HOSTDOWN = 64,      /* Host is down */
        HOSTUNREACH = 65,      /* No route to host */
        NOTEMPTY = 66,      /* Directory not empty */

        /* quotas & mush */
        PROCLIM = 67,      /* Too many processes */
        USERS = 68,      /* Too many users */
        DQUOT = 69,      /* Disc quota exceeded */

        /* Network File System */
        STALE = 70,      /* Stale NFS file handle */
        REMOTE = 71,      /* Too many levels of remote in path */
        BADRPC = 72,      /* RPC struct is bad */
        RPCMISMATCH = 73,      /* RPC version wrong */
        PROGUNAVAIL = 74,      /* RPC prog. not avail */
        PROGMISMATCH = 75,      /* Program version wrong */
        PROCUNAVAIL = 76,      /* Bad procedure for program */
        NOLCK = 77,      /* No locks available */
        NOSYS = 78,      /* Function not implemented */
        FTYPE = 79,      /* Inappropriate file type or format */
        AUTH = 80,      /* Authentication error */
        NEEDAUTH = 81,      /* Need authenticator */

        /* Intelligent device errors */
        PWROFF = 82,  /* Device power is off */
        DEVERR = 83,  /* Device error, e.g. paper out */
        OVERFLOW = 84,      /* Value too large to be stored in data type */

        /* Program loading errors */
        BADEXEC = 85,  /* Bad executable */
        BADARCH = 86,  /* Bad CPU type in executable */
        SHLIBVERS = 87,  /* Shared library version mismatch */
        BADMACHO = 88,  /* Malformed Macho file */
        CANCELED = 89,      /* Operation canceled */
        IDRM = 90,      /* Identifier removed */
        NOMSG = 91,      /* No message of desired type */
        ILSEQ = 92,      /* Illegal byte sequence */
        NOATTR = 93,      /* Attribute not found */
        BADMSG = 94,      /* Bad message */
        MULTIHOP = 95,      /* Reserved */
        NODATA = 96,      /* No message available on STREAM */
        NOLINK = 97,      /* Reserved */
        NOSR = 98,      /* No STREAM resources */
        NOSTR = 99,      /* Not a STREAM */
        PROTO = 100,     /* Protocol error */
        TIME = 101,     /* STREAM ioctl timeout */
        OPNOTSUPP = 102,     /* Operation not supported on socket */
        NOPOLICY = 103,     /* No such policy registered */
        NOTRECOVERABLE = 104,     /* State not recoverable */
        OWNERDEAD = 105,     /* Previous owner died */
        QFULL = 106,     /* Interface output queue is full */
        LAST = 106,     /* Must be equal largest errno */
    }
}
