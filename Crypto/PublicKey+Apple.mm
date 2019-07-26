//
// PublicKey+Apple.mm
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#import <Foundation/Foundation.h>
#include "Certificate.hh"
#include "PublicKey.hh"
#include "c4Private.h"
#include "Error.hh"
#include "Logging.hh"
#include "Defer.hh"
#include "SecureDigest.hh"

#include "mbedUtils.hh"
#include "pk.h"

#include <Security/Security.h>

/*  A WARNING to those working on this code: Apple's Keychain and security APIs are ☠️NASTY☠️.
    They vary a lot by platform, with many functions unavailable on iOS, or only available in
    alternate form. Some functions are on both platforms but behave differently, _especially_
    SecItemAdd/Delete/CopyMatching -- the underlying functionality of the Keychain is apparently
    very different between the two platforms. Some functions even behave differently on the
    iOS Simulator than on real devices!

    * Before using any new function, check its declaration carefully to make sure it's available
      cross-platform. If it isn't, there may be an alternative function for the other OS that
      you can use in an #ifdef.

    * Build and test on Mac, iOS simulator and iOS device after making any changes!

    * Make sure not to leak CoreFoundation objects. The DEFER macro from Defer.hh is
      useful for ensuring cleanup.

    * Similarly, avoid leaving Objective-C objects in the enclosing autorelease pool: there might
      not be one! Use @autoreleasepool{ } in methods that use Foundation APIs.

    * The _decrypt/_sign callbacks are `noexcept`, so errors should be returned not thrown.

    --Jens */

namespace litecore { namespace crypto {
    using namespace std;

    [[noreturn]] static void throwOSStatus(OSStatus err, const char *fnName, const char *what) {
        WarnError("%s (%s returned %d)", what, fnName, err);
        error::_throw(error::CryptoError, "%s (%s returned %d)", what, fnName, err);
    }

    static inline void checkOSStatus(OSStatus err, const char *fnName, const char *what) {
        if (_usuallyFalse(err != noErr))
            throwOSStatus(err, fnName, what);
    }

    static inline void warnCFError(CFErrorRef cfError, const char *fnName) {
        auto error = (__bridge NSError*)cfError;
        auto message = error.description;
        WarnError("%s failed: %s", fnName, message.UTF8String);
    }


    static CFTypeRef findInKeychain(NSDictionary *params) {
        CFTypeRef result = NULL;
        OSStatus err = SecItemCopyMatching((__bridge CFDictionaryRef)params, &result);
        if (err == errSecItemNotFound)
            return nullptr;
        else
            checkOSStatus(err, "SecItemCopyMatching", "Couldn't get an item from the Keychain");
        Assert(result != nullptr);
        return result;
    }


    struct ExpectingExceptions {
        ExpectingExceptions()    {++gC4ExpectExceptions;}
        ~ExpectingExceptions()   {--gC4ExpectExceptions;}
    };


#pragma mark - KEYPAIR:


    // Concrete subclass of KeyPair that uses Apple's Keychain and SecKey APIs.
    class KeychainKeyPair : public PersistentPrivateKey {
    public:
        /** The constructor adopts the key references; they're released in the destructor. */
        KeychainKeyPair(unsigned keySizeInBits, const string &label,
                        SecKeyRef publicKey, SecKeyRef privateKey)
        :PersistentPrivateKey(keySizeInBits, label)
        ,_publicKeyRef(publicKey)
        ,_privateKeyRef(privateKey)
        {
            Assert(publicKey && privateKey);
        }


        ~KeychainKeyPair() {
            CFRelease(_publicKeyRef);
            CFRelease(_privateKeyRef);
        }


        virtual alloc_slice publicKeyDERData() override {
            CFErrorRef error;
            CFDataRef data = SecKeyCopyExternalRepresentation(_publicKeyRef, &error);
            if (!data) {
                warnCFError(error, "SecKeyCopyExternalRepresentation");
                error::_throw(error::CryptoError, "Couldn't get the data of a public key");
            }
            alloc_slice result(CFDataGetBytePtr(data), CFDataGetLength(data));
            CFRelease(data);
            return result;
        }


        virtual int _decrypt(const void *input,
                             void *output,
                             size_t output_max_len,
                             size_t *output_len) noexcept override
        {
            // No exceptions may be thrown from this function!
            @autoreleasepool {
                NSData* data = slice(input, _keyLength).uncopiedNSData();
                CFErrorRef error;
                NSData* cleartext = CFBridgingRelease( SecKeyCreateDecryptedData(_privateKeyRef,
                                                             kSecKeyAlgorithmRSAEncryptionPKCS1,
                                                             (CFDataRef)data, &error) );
                if (!cleartext) {
                    warnCFError(error, "SecKeyCreateDecryptedData");
                    return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
                }
                *output_len = cleartext.length;
                if (*output_len > output_max_len)
                    return MBEDTLS_ERR_RSA_OUTPUT_TOO_LARGE;
                memcpy(output, cleartext.bytes, *output_len);
                return 0;
            }
        }


        virtual int _sign(int/*mbedtls_md_type_t*/ mbedDigestAlgorithm,
                          fleece::slice inputData,
                          void *outSignature) noexcept override
        {
            // No exceptions may be thrown from this function!
            @autoreleasepool {
                SecKeyAlgorithm digestAlgorithm;
                switch (mbedDigestAlgorithm) {
                case MBEDTLS_MD_SHA1:
                    digestAlgorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1; break;
                case MBEDTLS_MD_SHA256:
                    digestAlgorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256; break;
                default:
                    return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
                }

                NSData* data = inputData.uncopiedNSData();
                CFErrorRef error;
                NSData* sigData = CFBridgingRelease( SecKeyCreateSignature(_privateKeyRef,
                                                                 digestAlgorithm,
                                                                 (CFDataRef)data, &error) );
                if (!sigData) {
                    warnCFError(error, "SecKeyCreateSignature");
                    return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
                }
                Assert(sigData.length == _keyLength);
                memcpy(outSignature, sigData.bytes, _keyLength);
                return 0;
            }
        }

    private:
        SecKeyRef _publicKeyRef;
        SecKeyRef _privateKeyRef;
    };


#pragma mark - PUBLIC FACTORY METHODS:


    // Public function to generate a new key-pair
    Retained<PersistentPrivateKey> PersistentPrivateKey::generateRSA(unsigned keySizeInBits,
                                                                     const string &label)
    {
        @autoreleasepool {
            Log("Generating %d-bit RSA key-pair '%s' in Keychain", keySizeInBits, label.c_str());
            NSDictionary* params = @ {
                (id)kSecAttrKeyType:        (id)kSecAttrKeyTypeRSA,
                (id)kSecAttrKeySizeInBits:  @(keySizeInBits),
                (id)kSecAttrIsPermanent:    @YES,
                (id)kSecAttrLabel:          @(label.c_str()),
            };
            SecKeyRef publicKey, privateKey;
            ++gC4ExpectExceptions;
            OSStatus err = SecKeyGeneratePair((CFDictionaryRef)params, &publicKey, &privateKey);
            --gC4ExpectExceptions;
            checkOSStatus(err, "SecKeyGeneratePair", "Couldn't create a private key");

            return new KeychainKeyPair(keySizeInBits, label, publicKey, privateKey);
        }
    }


    Retained<PersistentPrivateKey> PersistentPrivateKey::load(const string &label) {
        @autoreleasepool {
            SecKeyRef privateKey = (SecKeyRef) findInKeychain(@{
                (id)kSecClass:              (id)kSecClassKey,
                (id)kSecAttrLabel:          @(label.c_str()),
                (id)kSecReturnRef:          @YES,
            });
            if (!privateKey)
                return nullptr;
            SecKeyRef publicKey = SecKeyCopyPublicKey(privateKey);
            auto keySizeInBits = unsigned(8 * SecKeyGetBlockSize(privateKey));
            return new KeychainKeyPair(keySizeInBits, label, publicKey, privateKey);
        }
    }


    bool PersistentPrivateKey::remove(const string &label) {
        @autoreleasepool {
            NSDictionary* params = @ {
                (id)kSecClass:              (id)kSecClassKey,
                (id)kSecAttrLabel:          @(label.c_str()),
                (id)kSecReturnRef:          @YES,
            };
            OSStatus err = SecItemDelete((CFDictionaryRef)params);
            if (err == errSecItemNotFound)
                return false;
            else
                checkOSStatus(err, "SecItemDelete", "Couldn't remove a key from the Keychain");
            return true;
        }
    }


#pragma mark - CERT:


    void Cert::makePersistent() {
        @autoreleasepool {
            Log("Adding certificate to Keychain for %s", subjectName().c_str());
            SecCertificateRef certRef = SecCertificateCreateWithData(kCFAllocatorDefault,
                                                                     (CFDataRef)data().copiedNSData());
            if (!certRef)
                throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT); // impossible?
            DEFER {CFRelease(certRef);};

            NSDictionary* params = @ {
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecValueRef:           (__bridge id)certRef,
                (id)kSecReturnRef:          @YES,
            };
            CFTypeRef result;
            ++gC4ExpectExceptions;
            OSStatus err = SecItemAdd((CFDictionaryRef)params, &result);
            --gC4ExpectExceptions;

            if (err == errSecDuplicateItem) {
                // A cert must already exist in the Keychain with the same label (common name).
                // Delete the existing one, then retry:
                CFStringRef commonName;
                checkOSStatus( SecCertificateCopyCommonName(certRef, &commonName),
                              "SecCertificateCopyCommonName",
                              "Couldn't replace a certificate in the Keychain");
                Log("...first removing existing certificate with label '%s'",
                    ((__bridge NSString*)commonName).UTF8String);
                NSDictionary* delParams = @ {
                    (id)kSecClass:              (id)kSecClassCertificate,
                    (id)kSecAttrLabel:          (__bridge id)commonName,
                };
                checkOSStatus( SecItemDelete((CFDictionaryRef)delParams),
                              "SecItemDelete",
                              "Couldn't replace a certificate in the Keychain");
                // Now retry:
                ++gC4ExpectExceptions;
                err = SecItemAdd((CFDictionaryRef)params, &result);
                --gC4ExpectExceptions;
            }

            checkOSStatus(err, "SecItemAdd", "Couldn't add a certificate to the Keychain");
            if (result)
                CFRelease(result);

#if 0
            // Dump the cert's Keychain attributes, for debugging:
            NSDictionary* attrs = CFBridgingRelease(findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecValueRef:           (__bridge id)certRef,
                (id)kSecReturnAttributes:   @YES,
            }));
            NSLog(@"Cert attributes: %@", attrs);
#endif
        }
    }


    Retained<Cert> Cert::load(PublicKey *subjectKey) {
        // The Keychain can look up a cert by the SHA1 digest of the raw form of its public key.
        @autoreleasepool {
            SHA1 digest(subjectKey->data(KeyFormat::Raw));
            NSData* certData = CFBridgingRelease(findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrPublicKeyHash:  [NSData dataWithBytes: &digest length: sizeof(digest)],
                (id)kSecReturnData:         @YES,
            }));
            return certData ? new Cert(slice(certData)) : nullptr;
        }
    }

} }
