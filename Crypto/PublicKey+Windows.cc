//
// PublicKey+Windows.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#pragma comment(lib, "ncrypt")

#include "Certificate.hh"
#include "PublicKey.hh"
#include "TLSContext.hh"
#include "Logging.hh"
#include "Error.hh"
#include "ParseDate.hh"
#include "StringUtil.hh"
#include "mbedUtils.hh"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/md5.h"
#include "TempArray.hh"
#include "Defer.hh"
#include "NumConversion.hh"
#include <Windows.h>
#include <ncrypt.h>
#include <wincrypt.h>
#include <functional>
#include <atomic>
#include <codecvt>
#include <chrono>

namespace litecore::crypto {
    using namespace std;
    using namespace net;
    using namespace fleece;

    static const DWORD LITECORE_ID_PROPERTY = CERT_FIRST_USER_PROP_ID;

    [[noreturn]]
    void throwSecurityStatus(SECURITY_STATUS err, const char *fnName, const char *what) {
        LogToAt(TLSLogDomain, Error, "%s (%s returned %d)", what, fnName, static_cast<int>(err));
        error::_throw(error::CryptoError, "%s (%s returned %d)", what, fnName, static_cast<int>(err));
    }

    [[noreturn]]
    void throwWincryptError(DWORD err, const char *fnName, const char *what) {
        LogToAt(TLSLogDomain, Error, "%s (%s returned %d)", what, fnName, err);
        error::_throw(error::CryptoError, "%s (%s returned %d)", what, fnName, err);
    }

    inline void checkSecurityStatus(SECURITY_STATUS err, const char *fnName, const char *what, 
        NCRYPT_HANDLE objectToFree = 0) {
        if(err != ERROR_SUCCESS) {
            if(objectToFree != 0) {
                NCryptFreeObject(objectToFree);
            }

            throwSecurityStatus(err, fnName, what);
        }
    }

    inline void checkWincryptBool(BOOL result, const char *fnName, const char *what) {
        if(!result) {
            const auto err = GetLastError();
            throwWincryptError(err, fnName, what);
        }
    }

    static PCCERT_CONTEXT toWinCert(Cert* cert) {
        PCCERT_CONTEXT resultCert = CertCreateCertificateContext(
            X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,
            (const BYTE *)cert->data().buf,
            narrow_cast<DWORD>(cert->data().size)
        );

        if(!resultCert) {
             throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT); 
        }

        return resultCert;
    }

    DWORD getBlockSize(NCRYPT_KEY_HANDLE hKey) {
        DWORD blockSize;
        DWORD dummy = sizeof(DWORD);
        const auto blockSizeResult = NCryptGetProperty(hKey, NCRYPT_BLOCK_LENGTH_PROPERTY, 
                                                       PBYTE(&blockSize), sizeof(DWORD), &dummy, 0);

        checkSecurityStatus(blockSizeResult, "NCryptGetProperty", "Couldn't get block size of key");
        return blockSize;
    }

    PCCERT_CONTEXT getWinCert(const string& id) {
        auto* store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_A,
            X509_ASN_ENCODING,
            NULL,
            CERT_SYSTEM_STORE_CURRENT_USER,
            "CA"
        );

        if(!store) {
            throwWincryptError(GetLastError(), "CertOpenStore", "Couldn't open system store");
        }

        DEFER {
            CertCloseStore(store, 0);
        };

        const DWORD prop = LITECORE_ID_PROPERTY;
        PCCERT_CONTEXT winCert = nullptr;
        void* enumContext = nullptr;
        do {
            winCert = CertFindCertificateInStore(
                store,
                X509_ASN_ENCODING,
                0,
                CERT_FIND_PROPERTY,
                &prop,
                winCert
            );

            if(!winCert) {
                break;
            }

            DWORD bytesNeeded = 0;
            BOOL success = CertGetCertificateContextProperty(
                winCert,
                LITECORE_ID_PROPERTY,
                nullptr,
                &bytesNeeded
            );

            if(!success) {
                CertFreeCertificateContext(winCert);
                throwWincryptError(GetLastError(), "CertGetCertificateContextProperty", "Couldn't read cert ID size");
            }

            void* idContent = malloc(bytesNeeded);
            DEFER {
                free(idContent);
            };

            success = CertGetCertificateContextProperty(
                winCert,
                LITECORE_ID_PROPERTY,
                idContent,
                &bytesNeeded
            );

            if(!success) {
                CertFreeCertificateContext(winCert);
                throwWincryptError(GetLastError(), "CertGetCertificateContextProperty", "Couldn't read cert ID");
            }

            const auto match = strcmp(id.c_str(), PCHAR(idContent)) == 0;
            if(match) {
                break;
            }
        } while(true);

        return winCert;
    }

    PCCERT_CHAIN_CONTEXT getCertChain(PCCERT_CONTEXT leaf) {
        CERT_CHAIN_PARA para = {
            sizeof(CERT_CHAIN_PARA),
        {USAGE_MATCH_TYPE_AND, {0, NULL}}
        };
        
        PCCERT_CHAIN_CONTEXT retVal;
        BOOL success = CertGetCertificateChain(
            HCCE_CURRENT_USER,
            leaf,
            NULL,
            NULL,
            &para,
            0,
            NULL,
            &retVal
        );

        checkWincryptBool(success, "CertGetCertificateChain", "Couldn't construct certificate chain");
        return retVal;
    }

    static int getChildCount(HCERTSTORE store, PCCERT_CONTEXT cert) {
        wchar_t subjectName[256];
        DWORD count = CertGetNameStringW(
            cert,
            CERT_NAME_SIMPLE_DISPLAY_TYPE,
            0,
            NULL,
            subjectName,
            256
        );

        PCCERT_CONTEXT childCert = nullptr;
        int certCount = -1;
        do {
            ++certCount;
            childCert = CertFindCertificateInStore(
                store,
                X509_ASN_ENCODING,
                0,
                CERT_FIND_ISSUER_STR_W,
                &subjectName,
                childCert
            );
        } while(childCert);

        return certCount;
    }

    static NCRYPT_PROV_HANDLE openStorageProvider() {
        NCRYPT_PROV_HANDLE hProvider;
        SECURITY_STATUS result = NCryptOpenStorageProvider(&hProvider, MS_PLATFORM_CRYPTO_PROVIDER, 
            0);
        if(result == ERROR_SUCCESS) {
            return hProvider;
        }

        // Fallback to default provider
        result = NCryptOpenStorageProvider(&hProvider, nullptr, 0);
        checkSecurityStatus(result, "NCryptOpenStorageProvider", "Couldn't open storage provider");
        return hProvider;
    }

    class NCryptPrivateKey : public PersistentPrivateKey
    {
    public:
        /** The constructor adopts the key reference; it's released in the destructor. */
        NCryptPrivateKey(unsigned keySizeInBits, NCRYPT_KEY_HANDLE keyPair)
            :PersistentPrivateKey(keySizeInBits)
            ,_keyPair(keyPair)
        {
            Assert(_keyPair);
        }

        ~NCryptPrivateKey() {
            NCryptFreeObject(_keyPair);
        }

        static alloc_slice publicKeyRawData(const NCRYPT_KEY_HANDLE hKey) {
            DWORD bytesNeeded = 0;

            SECURITY_STATUS result = NCryptExportKey(
                hKey,
                NULL,
                BCRYPT_RSAPUBLIC_BLOB,
                NULL,
                nullptr,
                0,
                &bytesNeeded,
                NCRYPT_SILENT_FLAG
            );
            checkSecurityStatus(result, "NCryptExportKey", "Couldn't get size of public key");

            TempArray(pkBytes, BYTE, bytesNeeded);
            auto* const pkInfo = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(pkBytes);
            result = NCryptExportKey(
                hKey,
                NULL,
                BCRYPT_RSAPUBLIC_BLOB,
                nullptr,
                pkBytes,
                bytesNeeded,
                &bytesNeeded,
                NCRYPT_SILENT_FLAG
            );
            checkSecurityStatus(result, "NCryptExportKey", "Couldn't get public key");

            BOOL encodeResult = CryptEncodeObject(
                X509_ASN_ENCODING,
                CNG_RSA_PUBLIC_KEY_BLOB,
                pkInfo,
                nullptr,
                &bytesNeeded
            );
            checkWincryptBool(encodeResult, "CryptEncodeObject", "Couldn't get bytes needed for ASN1");

            alloc_slice retVal(bytesNeeded);
            encodeResult = CryptEncodeObject(
                X509_ASN_ENCODING,
                CNG_RSA_PUBLIC_KEY_BLOB,
                pkInfo,
                PBYTE(retVal.buf),
                &bytesNeeded
            );
            checkWincryptBool(encodeResult, "CryptEncodeObject", "Couldn't encode to ASN1");

            return retVal;
        }

        alloc_slice publicKeyRawData() override {
            return publicKeyRawData(_keyPair);
        }

        alloc_slice publicKeyDERData() override {
            // Raw Data is also DER, but this is RSA-formatted DER
            // (ASN.1 is confusing...)
            return publicKey()->data(KeyFormat::DER);
        }

        void remove() override {
            NCRYPT_KEY_HANDLE old = _keyPair.exchange(0);
            if(!old) {
                return;
            }

            SECURITY_STATUS result = NCryptDeleteKey(old, 0);
            checkSecurityStatus(result, "NCryptDeleteKey", "Couldn't delete key");
        }

    protected:
        int _decrypt(const void* input, 
                     void* output, 
                     size_t output_max_len, 
                     size_t* output_len) noexcept override {
            const auto result = NCryptDecrypt(
                _keyPair,
                PBYTE(input),
                _keyLength,
                nullptr,
                PBYTE(output),
                narrow_cast<DWORD>(output_max_len),
                reinterpret_cast<DWORD*>(output_len),
                NCRYPT_PAD_PKCS1_FLAG
            );

            // No exceptions allowed in this function
            if(result != ERROR_SUCCESS) {
                LogToAt(TLSLogDomain, Error, "NCryptDecrypt failed to decrypt data (%d)", static_cast<int>(result));
                return result == NTE_BUFFER_TOO_SMALL
                    ? MBEDTLS_ERR_RSA_OUTPUT_TOO_LARGE
                    : MBEDTLS_ERR_RSA_PRIVATE_FAILED;
            }

            return 0;
        }

        int _sign(int /*mbedtls_md_type_t*/  mbedDigestAlgorithm, slice inputData, void* outSignature) noexcept override {
            // No exceptions may be thrown from this function!
            LogTo(TLSLogDomain, "Signing using NCrypt private key");
            static const LPCWSTR kDigestAlgorithmMap[9] = {
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                BCRYPT_SHA1_ALGORITHM,
                nullptr,
                BCRYPT_SHA256_ALGORITHM,
                BCRYPT_SHA384_ALGORITHM,
                BCRYPT_SHA512_ALGORITHM,
            };

            // 0 here is a special case (MBED_MD_NONE) which means "no digest algorithm"
            // This is not in the Windows documentation, but passing in a null digest algo
            // seems to do the trick.  Otherwise, null is considered an unsupported algorithm
            // for one of the other choices.
            LPCWSTR digestAlgorithm = nullptr;
            if (mbedDigestAlgorithm >= 1 && mbedDigestAlgorithm < 9)
                digestAlgorithm = kDigestAlgorithmMap[mbedDigestAlgorithm];
            if (mbedDigestAlgorithm != 0 && !digestAlgorithm) {
                LogToAt(TLSLogDomain, Warning, "Keychain private key: unsupported mbedTLS digest algorithm %d",
                     mbedDigestAlgorithm);
                return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
            }

            BCRYPT_PKCS1_PADDING_INFO padding = {
                digestAlgorithm
            };

            DWORD dummy = _keyLength;
            const auto result = NCryptSignHash(
                _keyPair,
                &padding,
                PBYTE(inputData.buf),
                narrow_cast<DWORD>(inputData.size),
                PBYTE(outSignature),
                _keyLength,
                &dummy,
                BCRYPT_PAD_PKCS1
            );
            if(result != ERROR_SUCCESS) {
                LogToAt(TLSLogDomain, Error, "NCryptSignHash failed to sign data (%d)", static_cast<int>(result));
                return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
            }

            return 0;
        }

        

    private:
        atomic<NCRYPT_KEY_HANDLE> _keyPair;
    };

    Retained<PersistentPrivateKey> PersistentPrivateKey::generateRSA(unsigned keySizeInBits) {
        LogTo(TLSLogDomain, "Generating %u-bit RSA key-pair in Keychain", keySizeInBits);
        char timestr[100] = "LiteCore ";
        wchar_t wtimestr[100];
        const time_t now = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
        FormatISO8601Date(timestr + strlen(timestr), now, false);
        const auto len = MultiByteToWideChar(CP_UTF8, 0, timestr, -1, wtimestr, 100);
        wtimestr[len] = 0;

        NCRYPT_PROV_HANDLE hProvider = openStorageProvider();
        NCRYPT_KEY_HANDLE hKey;
        SECURITY_STATUS result = NCryptCreatePersistedKey(hProvider, &hKey, NCRYPT_RSA_ALGORITHM, 
            wtimestr, 0, 0);

        NCryptFreeObject(hProvider);
        checkSecurityStatus(result, "NCryptCreatePersistedKey", "Couldn't create a private key");

        result = NCryptSetProperty(hKey, NCRYPT_LENGTH_PROPERTY, PBYTE(&keySizeInBits), sizeof(unsigned), 0);
        checkSecurityStatus(result, "NCryptSetProperty", "Couldn't set private key length");

        DWORD usage = NCRYPT_ALLOW_ALL_USAGES;
        result = NCryptSetProperty(hKey, NCRYPT_KEY_USAGE_PROPERTY, PBYTE(&usage), sizeof(DWORD), 0);
        checkSecurityStatus(result, "NCryptSetProperty", "Couldn't set private key usage", hKey);

        result = NCryptFinalizeKey(hKey, 0);
        checkSecurityStatus(result, "NCryptFinalizeKey", "Couldn't finalize key", hKey);

        return new NCryptPrivateKey(keySizeInBits, hKey);
    }

    Retained<PersistentPrivateKey> PersistentPrivateKey::withCertificate(Cert *cert) {
        return withPublicKey(cert->subjectPublicKey().get());
    }

    Retained<PersistentPrivateKey> PersistentPrivateKey::withPublicKey(PublicKey* publicKey) {
        NCRYPT_PROV_HANDLE hProvider = openStorageProvider();
        NCryptKeyName* next;
        NCRYPT_KEY_HANDLE hKey;
        void* enumState = nullptr;
        Retained<PersistentPrivateKey> retVal = nullptr;
        SECURITY_STATUS result;

        do {
            // Windows is terrible for this...it seems this is the only way to do this.
            // Instead of being able to tag the key with something, I have to go through
            // each one, derive the public key and compare
            result = NCryptEnumKeys(
                hProvider,
                NULL,
                &next,
                &enumState,
                0
            );

            if(result != ERROR_SUCCESS) {
                break;
            }

            SECURITY_STATUS openResult = NCryptOpenKey(
                hProvider,
                &hKey,
                next->pszName,
                0,
                0
            );

            if(openResult != ERROR_SUCCESS) {
                continue;
            }

            alloc_slice existingData;
            try {
                existingData = NCryptPrivateKey::publicKeyRawData(hKey);
            } catch(...) {
                LogToAt(TLSLogDomain, Warning, "Skipping unreadable key...");
            }

            if(existingData == publicKey->data(KeyFormat::Raw)) {
                retVal = new NCryptPrivateKey(getBlockSize(hKey) * 8, hKey);
                break;
            }
        } while(true);
        
        NCryptFreeObject(hProvider);
        if(retVal == nullptr) {
            if(result == NTE_NO_MORE_ITEMS) {
                LogToAt(TLSLogDomain, Error, "Unable to find matching private key!");
                error::_throw(error::CryptoError, "Unable to find matching private key!");
            }

            throwSecurityStatus(result, "NCryptEnumKeys", "Couldn't enumerate keys in storage");
        }

        return retVal;
    }

    void Cert::save(const string &persistentID, bool entireChain) {
        auto name = subjectName();
        LogTo(TLSLogDomain, "Adding a certificate chain with the id '%s' to the Keychain for '%.*s'",
            persistentID.c_str(), SPLAT(name));

        const auto* const winCert = getWinCert(persistentID);

        if(winCert) {
            CertFreeCertificateContext(winCert);
            throwSecurityStatus(CRYPT_E_EXISTS, "Cert::save", "A certificate already exists with the same persistentID");
        }

        auto* const store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_A,
            X509_ASN_ENCODING,
            NULL,
            CERT_SYSTEM_STORE_CURRENT_USER,
            "CA"
        );

        if(!store) {
            throwWincryptError(GetLastError(), "CertOpenSystemStore", "Couldn't open system store");
        }

        DEFER {
            CertCloseStore(store, 0);
        };

        for (Retained<Cert> cert = this; cert; cert = cert->next()) {
            const auto* const winCert = toWinCert(cert);
            DEFER {
                CertFreeCertificateContext(winCert);
            };

            if(cert == this) {
                CRYPT_DATA_BLOB idBlob {
                    narrow_cast<DWORD>(persistentID.size() + 1),
                    PBYTE(persistentID.c_str())
                };

                BOOL success = CertSetCertificateContextProperty(
                    winCert,
                    LITECORE_ID_PROPERTY,
                    0,
                    &idBlob
                );

                if(!success) {
                    throwWincryptError(GetLastError(), "CertSetCertificateContextProperty", 
                        "Couldn't set certificate ID");
                }
            }

            BOOL success = CertAddCertificateContextToStore(
                store,
                winCert,
                CERT_STORE_ADD_NEW,
                NULL
            );

            if(!success) {
                HRESULT err = GetLastError();
                if(err == CRYPT_E_EXISTS) {
                    // This is guaranteed not to be the first entry, since above an error would
                    // have been thrown, so it's safe to assume entireChain and continue
                    continue;
                }

                throwWincryptError(err, "CertAddCertificateContextToStore", "Couldn't add certificate");
            }

            if(!entireChain) {
                break;
            }
        }
    }

    Retained<Cert> Cert::loadCert(const std::string &persistentID) {
        LogTo(TLSLogDomain, "Loading a certificate chain with the id '%s' from the store", 
            persistentID.c_str());
            
        const auto* const winCert = getWinCert(persistentID);
        if(!winCert) {
            return nullptr;
        }

        Retained<Cert> cert = new Cert(slice(winCert->pbCertEncoded, winCert->cbCertEncoded));
        const auto* const winChain = getCertChain(winCert);

        DEFER {
            CertFreeCertificateContext(winCert);
            CertFreeCertificateChain(winChain);
        };

        const auto* const finalChain = winChain->rgpChain[winChain->cChain - 1];
        for(DWORD i = 1; i < finalChain->cElement; i++) {
            const auto* const element = finalChain->rgpElement[i];
            cert->append(new Cert(slice(element->pCertContext->pbCertEncoded, element->pCertContext->cbCertEncoded)));
        }

        return cert;
    }

    void Cert::deleteCert(const std::string &persistentID) {
        LogTo(TLSLogDomain, "Deleting a certificate with the id '%s' from the store",
                persistentID.c_str());

        const auto* const winCert = getWinCert(persistentID);
        if(!winCert) {
            return;
        }

        auto* const store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_A,
            X509_ASN_ENCODING,
            NULL,
            CERT_SYSTEM_STORE_CURRENT_USER,
            "CA"
        );

        const auto* const winChain = getCertChain(winCert);
        DEFER {
            CertFreeCertificateContext(winCert);
            CertFreeCertificateChain(winChain);
            CertCloseStore(store, 0);
        };

        const auto* const finalChain = winChain->rgpChain[winChain->cChain - 1];
        for(int i = finalChain->cElement - 1; i >= 0; i--) {
            const auto* const element = finalChain->rgpElement[i];

            // Delete the chain entry as long as it doesn't have more than 2 entries
            // in the store (itself and its direct child).  Unfortunately, deleting
            // a child cert doesn't appear to have an effect on "FindCertficate" until
            // the store is closed and re-opened.
            if(getChildCount(store, element->pCertContext) < 3) {
                checkWincryptBool(CertDeleteCertificateFromStore(element->pCertContext),
                    "CertDeleteCertificateFromStore", "Couldn't delete certificate");
            }
        }
    }

    Retained<Cert> Cert::load(PublicKey *subjectKey) {
        auto keyData = subjectKey->publicKeyData(KeyFormat::Raw);
        unsigned char hash[16];

        mbedtls_md5_context context;
        mbedtls_md5_init(&context);
        mbedtls_md5_starts(&context);
        mbedtls_md5_update(&context, (const unsigned char *)keyData.buf, keyData.size);
        mbedtls_md5_finish(&context, hash);
        mbedtls_md5_free(&context);

        CRYPT_HASH_BLOB hashBlob {
            16,
            hash
        };

        auto* const store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_A,
            X509_ASN_ENCODING,
            NULL,
            CERT_SYSTEM_STORE_CURRENT_USER,
            "CA"
        );

        auto winCert = CertFindCertificateInStore(
            store,
            X509_ASN_ENCODING,
            0,
            CERT_FIND_PUBKEY_MD5_HASH,
            &hashBlob,
            nullptr
        );

        DEFER {
            CertFreeCertificateContext(winCert);
            CertCloseStore(store, 0);
        };

        if(!winCert) {
            return nullptr;
        }

        return new Cert(slice(winCert->pbCertEncoded, winCert->cbCertEncoded));
    }

    Retained<PersistentPrivateKey> Cert::loadPrivateKey() {
        return PersistentPrivateKey::withCertificate(this);
    }

}
