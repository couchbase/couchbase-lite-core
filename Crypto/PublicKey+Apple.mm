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
#include "ParseDate.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedUtils.hh"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#pragma clang diagnostic pop

#include "fleece/slice.hh"
#include <Security/Security.h>

#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE     // Currently not defined for iOS

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
        WarnError("%s (%s returned %d)", what, fnName, int(err));
        error::_throw(error::CryptoError, "%s (%s returned %d)", what, fnName, int(err));
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


    static NSData* publicKeyHash(PublicKey *key NONNULL) {
        SHA1 digest(key->data(KeyFormat::Raw));
        return [NSData dataWithBytes: &digest length: sizeof(digest)];
    }


    static CFTypeRef CF_RETURNS_RETAINED findInKeychain(NSDictionary *params NONNULL) {
        CFTypeRef result = NULL;
        OSStatus err = SecItemCopyMatching((__bridge CFDictionaryRef)params, &result);
        if (err == errSecItemNotFound)
            return nullptr;
        else
            checkOSStatus(err, "SecItemCopyMatching", "Couldn't get an item from the Keychain");
        Assert(result != nullptr);
        return result;
    }


#pragma mark - KEYPAIR:


    // Concrete subclass of KeyPair that uses Apple's Keychain and SecKey APIs.
    class KeychainKeyPair : public PersistentPrivateKey {
    public:
        /** The constructor adopts the key references; they're released in the destructor. */
        KeychainKeyPair(unsigned keySizeInBits, SecKeyRef publicKey, SecKeyRef privateKey)
        :PersistentPrivateKey(keySizeInBits)
        ,_publicKeyRef(publicKey)
        ,_privateKeyRef(privateKey)
        {
            Assert(publicKey && privateKey);
        }


        ~KeychainKeyPair() {
            CFRelease(_publicKeyRef);
            CFRelease(_privateKeyRef);
        }


        virtual alloc_slice publicKeyRawData() override {
            CFErrorRef error;
            ++gC4ExpectExceptions;  // ignore internal C++ exceptions in Apple Security framework
            CFDataRef data = SecKeyCopyExternalRepresentation(_publicKeyRef, &error);
            --gC4ExpectExceptions;
            if (!data) {
                warnCFError(error, "SecKeyCopyExternalRepresentation");
                error::_throw(error::CryptoError, "Couldn't get the data of a public key");
            }
            alloc_slice result(data);
            CFRelease(data);
            return result;
        }


        virtual alloc_slice publicKeyDERData() override {
            // The Security framework on iOS doesn't seem to have a way to export a key in any
            // other format than raw (see above), so we have to detour through mbedTLS:
            return publicKey()->data(KeyFormat::DER);
        }


        virtual void remove() override {
            @autoreleasepool {
                NSDictionary* params = @ {
                    (id)kSecClass:              (id)kSecClassKey,
                    (id)kSecAttrKeyClass:       (id)kSecAttrKeyClassPrivate,
                    (id)kSecValueRef:           (__bridge id)_privateKeyRef,
                };
                checkOSStatus(SecItemDelete((CFDictionaryRef)params),
                              "SecItemDelete", "remove a key-pair from the Keychain");
            }
        }


        // Crypto operations:

        virtual int _decrypt(const void *input,
                             void *output,
                             size_t output_max_len,
                             size_t *output_len) noexcept override
        {
            // No exceptions may be thrown from this function!
            @autoreleasepool {
                Log("Decrypting using Keychain private key");
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
                if (*output_len > output_max_len)  // should never happen
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
            Log("Signing using Keychain private key");
            @autoreleasepool {
                // Map mbedTLS digest algorithm ID to SecKey algorithm ID:
                static const SecKeyAlgorithm kDigestAlgorithmMap[9] = {
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw,
                    NULL,
                    NULL,
                    NULL,
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1,
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA224,
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256,
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384,
                    kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512,
                };
                SecKeyAlgorithm digestAlgorithm = nullptr;
                if (mbedDigestAlgorithm >= 0 && mbedDigestAlgorithm < 9)
                    digestAlgorithm = kDigestAlgorithmMap[mbedDigestAlgorithm];
                if (!digestAlgorithm) {
                    Warn("Keychain private key: unsupported mbedTLS digest algorithm %d",
                         mbedDigestAlgorithm);
                    return MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE;
                }

                // Create the signature:
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
    Retained<PersistentPrivateKey> PersistentPrivateKey::generateRSA(unsigned keySizeInBits) {
        @autoreleasepool {
            Log("Generating %d-bit RSA key-pair in Keychain", keySizeInBits);
            char timestr[100] = "LiteCore ";
            fleece::FormatISO8601Date(timestr + strlen(timestr), time(nullptr)*1000, false);
            NSDictionary* params = @ {
                (id)kSecAttrKeyType:        (id)kSecAttrKeyTypeRSA,
                (id)kSecAttrKeySizeInBits:  @(keySizeInBits),
                (id)kSecAttrIsPermanent:    @YES,
                (id)kSecAttrLabel:          @(timestr),
            };
            SecKeyRef publicKey, privateKey;
            ++gC4ExpectExceptions;
            OSStatus err = SecKeyGeneratePair((CFDictionaryRef)params, &publicKey, &privateKey);
            --gC4ExpectExceptions;
            checkOSStatus(err, "SecKeyGeneratePair", "Couldn't create a private key");

            return new KeychainKeyPair(keySizeInBits, publicKey, privateKey);
        }
    }


    Retained<PersistentPrivateKey> PersistentPrivateKey::withCertificate(Cert *cert) {
        SecCertificateRef certRef = SecCertificateCreateWithData(kCFAllocatorDefault,
                                                        (CFDataRef)cert->data().copiedNSData());
        if (!certRef)
            throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT); // impossible?
        CFAutorelease(certRef);
        SecKeyRef publicKeyRef;
        checkOSStatus(SecCertificateCopyPublicKey(certRef, &publicKeyRef),
                      "SecCertificateCopyPublicKey", "get private key from keychain");

        SecIdentityRef identityRef;
        checkOSStatus(SecIdentityCreateWithCertificate(nullptr, certRef, &identityRef),
                      "SecIdentityCreateWithCertificate", "get private key from keychain");
        CFAutorelease(identityRef);
        SecKeyRef privateKeyRef;
        checkOSStatus(SecIdentityCopyPrivateKey(identityRef, &privateKeyRef),
                          "SecIdentityCopyPrivateKey", "get private key from keychain");
        auto keySize = unsigned(8 * SecKeyGetBlockSize(privateKeyRef));
        return new KeychainKeyPair(keySize, publicKeyRef, privateKeyRef);
    }


    Retained<PersistentPrivateKey> PersistentPrivateKey::withPublicKey(PublicKey* publicKey) {
        @autoreleasepool {
            // First look up a SecCertificateRef from the public-key digest. (We ought to be able
            // to look up a SecIdentityRef directly, but using kSecClassIdentity doesn't work;
            // the search matches all the identities in the Keychain...)
            auto certRef = (SecCertificateRef)findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrPublicKeyHash:  publicKeyHash(publicKey),
                (id)kSecReturnRef:          @YES,
            });
            if (!certRef)
                return nullptr;
            CFAutorelease(certRef);

            // Get the identity and then the private key:
            SecIdentityRef identityRef;
            checkOSStatus(SecIdentityCreateWithCertificate(nullptr, certRef, &identityRef),
                          "SecIdentityCreateWithCertificate", "get private key from keychain");
            CFAutorelease(identityRef);

            SecKeyRef privateKeyRef;
            checkOSStatus(SecIdentityCopyPrivateKey(identityRef, &privateKeyRef),
                          "SecIdentityCopyPrivateKey", "get private key from keychain");

            // Get the public key from the cert, not from the private key, because calling
            // SecKeyCopyPublicKey results in a key ref that doesn't allow its data to be read,
            // for some reason (bug?)

            SecKeyRef publicKeyRef;
            checkOSStatus(SecCertificateCopyPublicKey(certRef, &publicKeyRef),
                          "SecCertificateCopyPublicKey", "get private key from keychain");
            auto keySize = unsigned(8 * SecKeyGetBlockSize(privateKeyRef));
            return new KeychainKeyPair(keySize, publicKeyRef, privateKeyRef);
        }
    }


#pragma mark - CERT:


    void Cert::makePersistent() {
#if TARGET_OS_IPHONE
        error::_throw(error::Unimplemented, "Persistent certs/keys not working on iOS yet");
#else
        @autoreleasepool {
            auto name = subjectName();
            Log("Adding certificate to Keychain for %.*s", SPLAT(name));
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
                // Keychain can only have one cert with the same label (common name).
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
                (id)kSecReturnAttributes:   @YES
            }));
            NSLog(@"Cert attributes: %@", attrs);
#endif
        }
#endif
    }


    Retained<Cert> Cert::load(PublicKey *subjectKey) {
        // The Keychain can look up a cert by the SHA1 digest of the raw form of its public key.
        @autoreleasepool {
            NSData* certData = CFBridgingRelease(findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrPublicKeyHash:  publicKeyHash(subjectKey),
                (id)kSecReturnData:         @YES
            }));
            return certData ? new Cert(slice(certData)) : nullptr;
        }
    }


    Retained<PersistentPrivateKey> Cert::loadPrivateKey() {
        return PersistentPrivateKey::withCertificate(this);
    }

} }

#endif // PERSISTENT_PRIVATE_KEY_AVAILABLE
