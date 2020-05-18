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
        ++gC4ExpectExceptions;  // ignore internal C++ exceptions in Apple Security framework
        OSStatus err = SecItemCopyMatching((__bridge CFDictionaryRef)params, &result);
        --gC4ExpectExceptions;
        if (err == errSecItemNotFound)
            return nullptr;
        else
            checkOSStatus(err, "SecItemCopyMatching", "Couldn't get an item from the Keychain");
        return result;
    }


    static unsigned long getChildCertCount(SecCertificateRef parentCertRef) {
        NSDictionary* attrs = CFBridgingRelease(findInKeychain(@{
            (id)kSecClass:              (id)kSecClassCertificate,
            (id)kSecValueRef:           (__bridge id)parentCertRef,
            (id)kSecReturnAttributes:   @YES
        }));
        if (!attrs)
            return 0;
        
        NSString* subject = attrs[(id)kSecAttrSubject];
        Assert(subject);
        NSArray* children = CFBridgingRelease(findInKeychain(@{
            (id)kSecClass:              (id)kSecClassCertificate,
            (id)kSecAttrIssuer:         subject,
            (id)kSecMatchLimit:         (id)kSecMatchLimitAll
        }));
        return children.count;
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
            if (@available(macOS 10.12, iOS 10.0, *)) {
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
            } else {
                WarnError("Couldn't get the data of a public key: Not supported by macOS < 10.12 and iOS < 10.0");
                error::_throw(error::UnsupportedOperation, "Not supported by macOS < 10.12 and iOS < 10.0");
            }
        }


        virtual alloc_slice publicKeyDERData() override {
            // The Security framework on iOS doesn't seem to have a way to export a key in any
            // other format than raw (see above), so we have to detour through mbedTLS:
            return publicKey()->data(KeyFormat::DER);
        }


        virtual void remove() override {
            if (@available(macOS 10.12, iOS 10.0, *)) {
                @autoreleasepool {
                    // Try to get public key. If public key is not stored in the KeyChain, calling
                    // SecKeyCopyPublicKey on macOS will return null with internal C++ exceptions
                    // thrown in Apple Security framework:
                    ++gC4ExpectExceptions;
                    SecKeyRef publicKeyRef = SecKeyCopyPublicKey(_privateKeyRef);
                    --gC4ExpectExceptions;
                    
                    // Delete Public Key:
                    // Delete public key before private key, otherwise on macOS,
                    // errSecMissingEntitlement (-34018) will be returned:
                    if (publicKeyRef) {
                        CFAutorelease(publicKeyRef);
                        NSDictionary* params = @ {
                            (id)kSecClass:              (id)kSecClassKey,
                            (id)kSecAttrKeyClass:       (id)kSecAttrKeyClassPublic,
                            (id)kSecValueRef:           (__bridge id)publicKeyRef,
                        };
                        OSStatus status = SecItemDelete((CFDictionaryRef)params);
                        if (status != errSecSuccess && status != errSecItemNotFound)
                            checkOSStatus(status, "SecItemDelete", "Couldn't remove a public key from the Keychain");
                    }
                    
                    // Delete Private Key:
                    NSDictionary* params = @ {
                        (id)kSecClass:              (id)kSecClassKey,
                        (id)kSecAttrKeyClass:       (id)kSecAttrKeyClassPrivate,
                        (id)kSecValueRef:           (__bridge id)_privateKeyRef,
                    };
                    OSStatus status = SecItemDelete((CFDictionaryRef)params);
                    if (status != errSecSuccess && status != errSecItemNotFound)
                        checkOSStatus(status, "SecItemDelete", "Couldn't remove a private key from the Keychain");
                }
            } else {
                WarnError("Couldn't remove keys: Not supported by macOS < 10.12 and iOS < 10.0");
                throwMbedTLSError(MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE);
            }
        }


        // Crypto operations:

        virtual int _decrypt(const void *input,
                             void *output,
                             size_t output_max_len,
                             size_t *output_len) noexcept override
        {
            // No exceptions may be thrown from this function!
            if (@available(macOS 10.12, iOS 10.0, *)) {
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
            } else {
                WarnError("Couldn't decrypt using Keychain private key: Not supported by macOS < 10.12 and iOS < 10.0");
                return MBEDTLS_ERR_RSA_UNSUPPORTED_OPERATION;
            }
        }


        virtual int _sign(int/*mbedtls_md_type_t*/ mbedDigestAlgorithm,
                          fleece::slice inputData,
                          void *outSignature) noexcept override
        {
            // No exceptions may be thrown from this function!
            if (@available(macOS 10.12, iOS 10.0, *)) {
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
            } else {
                WarnError("Couldn't sign using Keychain private key: Not supported by macOS < 10.12 and iOS < 10.0");
                return MBEDTLS_ERR_RSA_UNSUPPORTED_OPERATION;
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
        if (@available(macOS 10.12, iOS 10, *)) {
            @autoreleasepool {
                SecCertificateRef certRef = SecCertificateCreateWithData(kCFAllocatorDefault,
                                                                         (CFDataRef)cert->data().copiedNSData());
                if (!certRef)
                    throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT); // impossible?
                CFAutorelease(certRef);
                
                // Get public key from the certificate using trust:
                SecTrustRef trustRef;
                SecPolicyRef policyRef = SecPolicyCreateBasicX509();
                checkOSStatus(SecTrustCreateWithCertificates(certRef, policyRef, &trustRef),
                              "SecTrustCreateWithCertificates", "Couldn't create trust to get public key");
                SecKeyRef publicKeyRef = SecTrustCopyPublicKey(trustRef);
                CFRelease(policyRef);
                CFRelease(trustRef);
                
                if (!publicKeyRef)
                    return nullptr;
                
                // Get public key hash from kSecAttrApplicationLabel attribute:
                // See: https://developer.apple.com/documentation/security/ksecattrapplicationlabel
                NSDictionary* attrs = CFBridgingRelease(SecKeyCopyAttributes(publicKeyRef));
                NSData* publicKeyHash = [attrs objectForKey: (id)kSecAttrApplicationLabel];
                if (!publicKeyHash) {
                    CFRelease(publicKeyRef);
                    throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT);
                }
                
                // Lookup private key by using the public key hash:
                auto privateKeyRef = (SecKeyRef)findInKeychain(@{
                    (id)kSecClass:                  (id)kSecClassKey,
                    (id)kSecAttrKeyType:            (id)kSecAttrKeyTypeRSA,
                    (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPrivate,
                    (id)kSecAttrApplicationLabel:   publicKeyHash,
                    (id)kSecReturnRef:              @YES
                });
                if (!privateKeyRef) {
                    CFRelease(publicKeyRef);
                    return nullptr;
                }
                
                auto keySize = unsigned(8 * SecKeyGetBlockSize(privateKeyRef));
                return new KeychainKeyPair(keySize, publicKeyRef, privateKeyRef);
            }
        } else {
            WarnError("Couldn't get private key using certificate: Not supported by macOS < 10.12 and iOS < 10.0");
            throwMbedTLSError(MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE);
        }
    }


    Retained<PersistentPrivateKey> PersistentPrivateKey::withPublicKey(PublicKey* publicKey) {
        @autoreleasepool {
            if (@available(macOS 10.12, iOS 10.0, *)) {
                // Lookup private key by using the public key hash:
                auto privateKeyRef = (SecKeyRef)findInKeychain(@{
                    (id)kSecClass:                  (id)kSecClassKey,
                    (id)kSecAttrKeyType:            (id)kSecAttrKeyTypeRSA,
                    (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPrivate,
                    (id)kSecAttrApplicationLabel:   publicKeyHash(publicKey),
                    (id)kSecReturnRef:              @YES
                });
                if (!privateKeyRef)
                    return nullptr;
                
                ++gC4ExpectExceptions;
                auto publicKeyRef = SecKeyCopyPublicKey(privateKeyRef);
                --gC4ExpectExceptions;
                if (!publicKeyRef) {
                    // If the public key is not in the KeyChain, SecKeyCopyPublicKey() will return null.
                    // Create a SecKeyRef directly from the publicKey data instead:
                    NSDictionary* attrs = @{
                        (id)kSecAttrKeyType:            (id)kSecAttrKeyTypeRSA,
                        (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPublic,
                        (id)kSecAttrKeySizeInBits:      @2048
                    };
                    CFErrorRef error;
                    publicKeyRef = SecKeyCreateWithData((CFDataRef)publicKey->data().copiedNSData(),
                                                        (CFDictionaryRef)attrs, &error);
                    if (!publicKeyRef) {
                        CFRelease(privateKeyRef);
                        warnCFError(error, "SecKeyCreateWithData");
                        throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT);
                    }
                }
                auto keySize = unsigned(8 * SecKeyGetBlockSize(privateKeyRef));
                return new KeychainKeyPair(keySize, publicKeyRef, privateKeyRef);
            } else {
                WarnError("Couldn't get private key using public key: Not supported by macOS < 10.12 and iOS < 10.0");
                throwMbedTLSError(MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE);
            }
        }
    }


#pragma mark - CERT:


    // Save the certificate chain into the Keychain.
    // 1. The persistentID will be assigned as a label to the leaf cert.
    // 2. Do not allow to save the cert chain with duplicate persistentID.
    // 3. [Apple Specific] Do not allow to save the duplicate leaf cert (even using a different persistentID)
    //    as the Keychain doesn't allow the duplicate certs (same serial and issuer).
    // 4. [Apple Specific] Ignore the Keychain's duplicate error for non-leaf certs to allow to save
    //    the cert chains that share some intermediate certs.
    void Cert::save(const std::string &persistentID, bool entireChain) {
        @autoreleasepool {
            auto name = subjectName();
            Log("Adding a certificate chain with the id '%s' to the Keychain for '%.*s'",
                persistentID.c_str(), SPLAT(name));
            
            NSString* label = [NSString stringWithCString: persistentID.c_str() encoding: NSUTF8StringEncoding];
            CFTypeRef ref = findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrLabel:          label,
                (id)kSecReturnRef:          @YES
            });
            if (ref) {
                CFRelease(ref);
                checkOSStatus(errSecDuplicateItem,
                              "Cert::save",
                              "A certificate already exists with the same persistentID");
            }
            
            for (Retained<Cert> cert = this; cert; cert = cert->next()) {
                SecCertificateRef certRef = SecCertificateCreateWithData(kCFAllocatorDefault,
                                                                         (CFDataRef)cert->data().copiedNSData());
                if (!certRef)
                    throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT);
                CFAutorelease(certRef);

                NSMutableDictionary* params = [NSMutableDictionary dictionaryWithDictionary: @{
                    (id)kSecClass:              (id)kSecClassCertificate,
                    (id)kSecValueRef:           (__bridge id)certRef,
                    (id)kSecReturnRef:          @YES
                }];
                
                if (cert == this) {
                    // Set the label to the leaf cert:
                    params[id(kSecAttrLabel)] = label;
                }
                
                CFTypeRef result;
                ++gC4ExpectExceptions;
                OSStatus status = SecItemAdd((CFDictionaryRef)params, &result);
                --gC4ExpectExceptions;
                
                if (result)
                    CFAutorelease(result);
                
                if (status == errSecDuplicateItem && cert != this) {
                    // Ignore duplicates as it might be referenced by the other certificates
                    Log("Ignore adding the certificate to the Keychain for '%.*s' as duplicated",
                        SPLAT(cert->subjectName()));
                    continue;
                } else
                    checkOSStatus(status, "SecItemAdd", "Couldn't add a certificate to the Keychain");
                
            #if TARGET_OS_OSX
                // Workaround for macOS that the label is not set as specified
                // when adding the certificate to the Keychain.
                NSArray* items = (__bridge NSArray*)result;
                Assert(items.count > 0);
                if (cert == this) {
                    NSDictionary* certQuery = @{
                        (id)kSecClass:              (id)kSecClassCertificate,
                        (id)kSecValueRef:           items[0]
                    };
                    
                    NSDictionary* updatedAttrs = @{
                        (id)kSecClass:              (id)kSecClassCertificate,
                        (id)kSecValueRef:           items[0],
                        (id)kSecAttrLabel:          label
                    };
                    
                    ++gC4ExpectExceptions;
                    checkOSStatus(SecItemUpdate((CFDictionaryRef)certQuery, (CFDictionaryRef)updatedAttrs),
                                  "SecItemUpdate",
                                  "Couldn't update the label to a certificate in Keychain");
                    --gC4ExpectExceptions;
                }
            #endif
                
                if (!entireChain)
                    break;
            }
        }
    }


    fleece::Retained<Cert> Cert::loadCert(const std::string &persistentID) {
        @autoreleasepool {
            Log("Loading a certificate chain with the id '%s' from the Keychain", persistentID.c_str());
            
            NSString* label = [NSString stringWithCString: persistentID.c_str()
                                                 encoding: NSUTF8StringEncoding];
            NSDictionary* attrs = CFBridgingRelease(findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrLabel:          label,
                (id)kSecReturnRef:          @YES,
                (id)kSecReturnData:         @YES
            }));
            if (!attrs)
                return nullptr;
            
            NSData* certData = attrs[(id)kSecValueData];
            Assert(certData);
            Retained<Cert> cert = new Cert(slice(certData));
            
            // Create and evaluate trust to get certificate chain:
            SecCertificateRef certRef = (__bridge SecCertificateRef)attrs[(id)kSecValueRef];
            Assert(certRef);
            
            SecPolicyRef policyRef = SecPolicyCreateBasicX509();
            CFAutorelease(policyRef);
            
            SecTrustRef trustRef;
            checkOSStatus(SecTrustCreateWithCertificates(certRef, policyRef, &trustRef),
                          "SecTrustCreateWithCertificates",
                          "Couldn't create a trust to get certificate chain");
            CFAutorelease(trustRef);
            
            SecTrustResultType result; // Result will be ignored.
            checkOSStatus(SecTrustEvaluate(trustRef, &result),
                          "SecTrustEvaluate",
                          "Couldn't evaluate the trust to get certificate chain" );
            
            CFIndex count = SecTrustGetCertificateCount(trustRef);
            Assert(count > 0);
            for (CFIndex i = 1; i < count; i++) {
                SecCertificateRef ref = SecTrustGetCertificateAtIndex(trustRef, i);
                NSData* data = (NSData*) CFBridgingRelease(SecCertificateCopyData(ref));
                cert->append(new Cert(slice(data)));
            }
            
            return cert;
        }
    }


    void Cert::deleteCert(const std::string &persistentID) {
        @autoreleasepool {
            Log("Deleting a certificate chain with the id '%s' from the Keychain",
                persistentID.c_str());
            
            NSString* label = [NSString stringWithCString: persistentID.c_str()
                                                 encoding: NSUTF8StringEncoding];
            SecCertificateRef certRef = (SecCertificateRef)findInKeychain(@{
                (id)kSecClass:              (id)kSecClassCertificate,
                (id)kSecAttrLabel:          label,
                (id)kSecReturnRef:          @YES
            });
            if (!certRef)
                return;
            CFAutorelease(certRef);
            
            // Create and evaluate trust to get certificate chain:
            SecPolicyRef policyRef = SecPolicyCreateBasicX509();
            CFAutorelease(policyRef);
            
            SecTrustRef trustRef;
            checkOSStatus(SecTrustCreateWithCertificates(certRef, policyRef, &trustRef),
                          "SecTrustCreateWithCertificates",
                          "Couldn't create a trust to get certificate chain");
            CFAutorelease(trustRef);
            
            SecTrustResultType result; // Result will be ignored.
            checkOSStatus(SecTrustEvaluate(trustRef, &result),
                          "SecTrustEvaluate",
                          "Couldn't evaluate the trust to get certificate chain");
            
            CFIndex count = SecTrustGetCertificateCount(trustRef);
            Assert(count > 0);
            for (CFIndex i = count - 1; i >= 0; i--) {
                SecCertificateRef ref = SecTrustGetCertificateAtIndex(trustRef, i);
                if (getChildCertCount(ref) < 2) {
                    NSDictionary* params = @{
                        (id)kSecClass:              (id)kSecClassCertificate,
                        (id)kSecValueRef:           (__bridge id)ref
                    };
                    checkOSStatus(SecItemDelete((CFDictionaryRef)params),
                                  "SecItemDelete",
                                  "Couldn't delete a certificate from the Keychain");
                }
            }
        }
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
