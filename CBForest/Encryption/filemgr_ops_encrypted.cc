//
//  filemgr_ops_encrypted.cc
//  CBForest
//
//  Created by Jens Alfke on 7/27/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "filemgr_ops_encrypted.h"
#include "filemgr.h"
#include "filemgr_ops.h"
#include "LogInternal.hh"
#include <vector>
#include <unordered_map>

#include <Security/Security.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>


#define ENABLE_LOG 0    // To get log messages, set to 1 and set LogLevel to kInfo
#define LogFileMgr(MESSAGE...)        if(!ENABLE_LOG) ; else _Log(kInfo, MESSAGE)


extern struct filemgr_ops * get_linux_filemgr_ops();


namespace forestdb {


#define FAKE_ENCRYPTION 0


static const size_t kPageSize = 4096;       // Must match the page size used by ForestDB!


class EncryptedFileMgr {
public:

    static void registerKey(const char *pathname, EncryptionKey key)
    {
        spin_lock(&sLockRegisteredKeys);
        sRegisteredKeys[pathname] = key;
        spin_unlock(&sLockRegisteredKeys);
    }

    static EncryptedFileMgr* get(int fd)
    {
        assert(fd >= kBaseFakeFD);
        rw_spin_read_lock(&sLockOpenFiles);
        EncryptedFileMgr* mgr = sOpenFiles[fd - kBaseFakeFD];
        rw_spin_read_unlock(&sLockOpenFiles);
        assert(mgr != NULL);
        return mgr;
    }

    static EncryptedFileMgr* get(std::string path)
    {
        EncryptedFileMgr* result = NULL;
        rw_spin_read_lock(&sLockOpenFiles);
        for (auto fileP=sOpenFiles.begin(); fileP != sOpenFiles.end(); ++fileP)
            if (*fileP && (*fileP)->_path == path) {
                result = *fileP;
                break;
            }
        rw_spin_read_unlock(&sLockOpenFiles);
        return result;
    }

    // I/O operations:

    static int open(const char *pathname, int flags, mode_t mode)
    {
        int fd = defaultOps->open(pathname, flags, mode);
        if (fd < 0) {
            LogFileMgr("%d <- OPEN %s (failed)", fd, pathname);
            return fd;
        }
        EncryptedFileMgr* mgr = new EncryptedFileMgr(fd, pathname, flags, mode);
        LogFileMgr("%d <- OPEN %s (real FD is %d) encrypted=%d",
                   mgr->_fakeFD, pathname, fd, mgr->_encrypted);
        return mgr->_fakeFD;
    }
    
    int close()
    {
        LogFileMgr("%d:   CLOSE", _fakeFD);
        int result = defaultOps->close(_realFD);
        if (result >= 0)
            delete this;
        return result;
    }

    ssize_t pwrite(void *buf, size_t count, cs_off_t offset)
    {
        LogFileMgr("%d:   PWRITE %zd at %zd", _fakeFD, count, offset);
        uint8_t encryptedBuf[count];
        if (_encrypted) {
            if (offset % kPageSize != 0 || count != kPageSize) {
                return FDB_RESULT_INVALID_IO_PARAMS;
            }
            if (!crypt(encryptedBuf, buf, count, offset / kPageSize, true))
                return FDB_RESULT_ENCRYPTION_ERROR;
            buf = encryptedBuf;
        }
        return defaultOps->pwrite(_realFD, buf, count, offset);
    }

    ssize_t pread(void *buf, size_t count, cs_off_t offset)
    {
        LogFileMgr("%d:   PREAD %zd from %zd", _fakeFD, count, offset);
        ssize_t result = defaultOps->pread(_realFD, buf, count, offset);
        if (_encrypted && result >= 0) {
            if (offset % kPageSize != 0 || count != kPageSize) {
                return FDB_RESULT_INVALID_IO_PARAMS;
            } else if (result != count) {
                return FDB_RESULT_READ_FAIL;
            }
            if (!crypt(buf, buf, result, offset / kPageSize, false))
                return FDB_RESULT_ENCRYPTION_ERROR;
        }
        return result;
    }

    cs_off_t goto_eof()
    {
        LogFileMgr("%d:   GOTO EOF", _fakeFD);
        return defaultOps->goto_eof(_realFD);
    }

    int fsync()
    {
        LogFileMgr("%d:   FSYNC", _fakeFD);
        return defaultOps->fsync(_realFD);
    }
    
    int fdatasync()
    {
        LogFileMgr("%d:   FDATASYNC", _fakeFD);
        return defaultOps->fdatasync(_realFD);
    }

    static const filemgr_ops* const defaultOps;

private:

    EncryptedFileMgr(int realFD, std::string pathname, int flags, mode_t mode)
    :_path(pathname),
     _realFD(realFD),
     _fakeFD(createFakeFD()),
     _encrypted(false)
    {
        // Look up the key registered for this path:
        spin_lock(&sLockRegisteredKeys);
        auto keyP = sRegisteredKeys.find(pathname);
        if (keyP != sRegisteredKeys.end()) {
            _encrypted = true;
            _key = keyP->second;
            sRegisteredKeys.erase(keyP);
        }
        spin_unlock(&sLockRegisteredKeys);

        if (!_encrypted) {
            // KLUDGE: ForestDB appends ".1", ".2", etc. for temporary files used for compaction.
            std::string basePath = removeCompactionSuffix(pathname);
            if (basePath.length() > 0) {
                EncryptedFileMgr* mgr = get(basePath);
                if (mgr) {
                    _key = mgr->_key;
                    _encrypted = true;
                }
            }
        }

        if (_encrypted) {
            // Generate a secondary encryption key to use for initialization vectors (IVs)
            assert(sizeof(_IVKey) == CC_SHA256_DIGEST_LENGTH);
            ::CC_SHA256(&_key, sizeof(_key), _IVKey.bytes);
        }
    }

    ~EncryptedFileMgr()
    {
        // Remove myself from the open file list:
        rw_spin_write_lock(&sLockOpenFiles);
        assert(sOpenFiles[_fakeFD - kBaseFakeFD] == this);
        sOpenFiles[_fakeFD - kBaseFakeFD] = NULL;
        rw_spin_write_unlock(&sLockOpenFiles);
    }

    static const int kBaseFakeFD = 0x10000; // Where numbering of fake file descriptors starts

    // Allocates a fake file descriptor, adding the receiver to the sOpenFiles table
    int createFakeFD()
    {
        rw_spin_write_lock(&sLockOpenFiles);
        size_t size = sOpenFiles.size();
        size_t fd;
        for (fd = 0; fd < size; fd++)
            if (sOpenFiles[fd] == NULL) {
                sOpenFiles[fd] = this;
                break;
            }
        if (fd == size) {
            sOpenFiles.push_back(this);
        }
        rw_spin_write_unlock(&sLockOpenFiles);
        return kBaseFakeFD + (int)fd;
    }

    // Encrypts/decrypts `count` bytes from `src`, writing to `dst`. (It's ok if `src`==`dst`.)
    // `blockNo` is the block index, used as a nonce to alter the encryption for each block.
    bool crypt(void *dst, const void *src, size_t count, uint64_t blockNo, bool encrypt)
    {
        LogFileMgr("%d:      %sCRYPT block #%llu (%zd bytes)",
                _fakeFD, (encrypt ?"EN" :"DE"), blockNo, count);
        assert(_encrypted);
        // Derive an IV using the Encrypted Salt-Sector Initialization Value (ESSIV) algorithm
        // (see https://en.wikipedia.org/wiki/Disk_encryption_theory )
        // The IV is computed by encrypting the block number using _IVKey (a digest of the key.)
        uint8_t iv[kCCBlockSizeAES128] = {0};
        uint64_t bigBlockNo = _endian_encode(blockNo);
        memcpy(&iv, &bigBlockNo, sizeof(bigBlockNo));

        size_t bytesEncrypted;
        CCCryptorStatus status;
        status = ::CCCrypt(kCCEncrypt,
                           kCCAlgorithmAES,
                           0,
                           _IVKey.bytes, sizeof(_IVKey.bytes),
                           NULL,
                           &iv, sizeof(iv),
                           &iv, sizeof(iv),
                           &bytesEncrypted);
        if (status != kCCSuccess)
            return false;

        // Now encrypt the block using the main key and the IV:
        status = ::CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt,
                                           kCCAlgorithmAES,
                                           0,
                                           _key.bytes, sizeof(_key.bytes),
                                           &iv,
                                           src, count,
                                           dst, count,
                                           &bytesEncrypted);
        return status == kCCSuccess && bytesEncrypted == count;
    }

    // If the file ends with '.' followed by digits, removes the suffix and returns the base name.
    // Else returns an empty string.
    static std::string removeCompactionSuffix(std::string pathname) {
        ssize_t i = pathname.length() - 1;
        bool anyDigits = false;
        while (i >= 0 && isdigit(pathname[i])) {
            anyDigits = true;
            --i;
        }
        if (anyDigits && i > 0 && pathname[i] == '.') {
            return pathname.substr(0, i);
        }
        return "";
    }

    // Data members:
    const std::string _path;        // Path of the file
    const int _realFD;              // Real Unix file descriptor
    const int _fakeFD;              // Fake file descriptor returned to caller
    bool _encrypted;                // Is this file encrypted?
    EncryptionKey _key, _IVKey;     // Encryption key, and secondary key for generating IVs

    // Map of file paths to registered keys:
    static std::unordered_map<std::string, EncryptionKey> sRegisteredKeys;
    static spin_t sLockRegisteredKeys;

    // Vector mapping fake file descriptors to their EncryptedFileMgr instances:
    static std::vector<EncryptedFileMgr*> sOpenFiles;
    static rw_spin_t sLockOpenFiles;
};


const filemgr_ops* const EncryptedFileMgr::defaultOps = get_linux_filemgr_ops();

std::unordered_map<std::string, EncryptionKey> EncryptedFileMgr::sRegisteredKeys;
std::vector<EncryptedFileMgr*> EncryptedFileMgr::sOpenFiles;
spin_t EncryptedFileMgr::sLockRegisteredKeys;
rw_spin_t EncryptedFileMgr::sLockOpenFiles;


#pragma mark - OPS:


static int _filemgr_encrypted_open(const char *pathname, int flags, mode_t mode)
{
    return EncryptedFileMgr::open(pathname, flags, mode);
}

static ssize_t _filemgr_encrypted_pwrite(int fd, void *buf, size_t count, cs_off_t offset)
{
    return EncryptedFileMgr::get(fd)->pwrite(buf, count, offset);
}

static ssize_t _filemgr_encrypted_pread(int fd, void *buf, size_t count, cs_off_t offset)
{
    return EncryptedFileMgr::get(fd)->pread(buf, count, offset);
}

static int _filemgr_encrypted_close(int fd)
{
    return EncryptedFileMgr::get(fd)->close();
}

static cs_off_t _filemgr_encrypted_goto_eof(int fd)
{
    return EncryptedFileMgr::get(fd)->goto_eof();
}

static cs_off_t _filemgr_encrypted_file_size(const char *filename)
{
    return EncryptedFileMgr::defaultOps->file_size(filename);
}

static int _filemgr_encrypted_fsync(int fd)
{
    return EncryptedFileMgr::get(fd)->fsync();
}

static int _filemgr_encrypted_fdatasync(int fd)
{
    return EncryptedFileMgr::get(fd)->fdatasync();
}

static void _filemgr_encrypted_get_errno_str(char *buf, size_t size) {
    return EncryptedFileMgr::defaultOps->get_errno_str(buf, size);
}

static int _filemgr_encrypted_aio_init(struct async_io_handle *aio_handle)
{
    return FDB_RESULT_AIO_NOT_SUPPORTED;
}

static int _filemgr_encrypted_is_cow_support(int src_fd, int dst_fd)
{
    return FDB_RESULT_INVALID_ARGS;
}

static struct filemgr_ops encrypted_ops = {
    _filemgr_encrypted_open,
    _filemgr_encrypted_pwrite,
    _filemgr_encrypted_pread,
    _filemgr_encrypted_close,
    _filemgr_encrypted_goto_eof,
    _filemgr_encrypted_file_size,
    _filemgr_encrypted_fdatasync,
    _filemgr_encrypted_fsync,
    _filemgr_encrypted_get_errno_str,
    // Async I/O operations
    _filemgr_encrypted_aio_init,
    NULL,
    NULL,
    NULL,
    NULL,
    _filemgr_encrypted_is_cow_support,
    NULL
};

extern "C" {
    // declared in filemgr_ops.h
    struct filemgr_ops * get_filemgr_ops()
    {
        return &encrypted_ops;
    }

    void fdb_registerEncryptionKey(const char *pathname, EncryptionKey key) {
        EncryptedFileMgr::registerKey(pathname, key);
    }

    EncryptionKey fdb_randomEncryptionKey() {
        EncryptionKey key;
        ::SecRandomCopyBytes(kSecRandomDefault, sizeof(key), key.bytes);
        return key;
    }

}

}
