//
// PublicKey+Apple.mm
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#import <Foundation/Foundation.h>
#include "Certificate.hh"
#include "PublicKey.hh"
#include "TLSContext.hh"
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

namespace litecore { namespace crypto {
    using namespace std;
    using namespace net;

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

    [[noreturn]] __unused
    static void throwOSStatus(OSStatus err, const char *fnName, const char *what) {
        LogError(TLSLogDomain, "%s (%s returned %d)", what, fnName, int(err));
        error::_throw(error::CryptoError, "%s (%s returned %d)", what, fnName, int(err));
    }

    __unused static inline void checkOSStatus(OSStatus err, const char *fnName, const char *what) {
        if (_usuallyFalse(err != noErr))
            throwOSStatus(err, fnName, what);
    }

    __unused
    static inline void warnOSStatusError(OSStatus err, const char *fnName, const char *what) {
        if (_usuallyFalse(err != noErr)) {
            LogError(TLSLogDomain, "%s (%s returned %d)", what, fnName, int(err));
        }
    }

    __unused static inline void warnCFError(CFErrorRef cfError, const char *fnName) {
        auto error = (__bridge NSError*)cfError;
        auto message = error.description;
        LogError(TLSLogDomain, "%s failed: %s", fnName, message.UTF8String);
    }


    __unused static NSData* publicKeyHash(PublicKey *key NONNULL) {
        SHA1 digest(key->data(KeyFormat::Raw));
        return [NSData dataWithBytes: &digest length: sizeof(digest)];
    }


    __unused static CFTypeRef CF_RETURNS_RETAINED findInKeychain(NSDictionary *params NONNULL) {
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


    __unused static unsigned long getChildCertCount(SecCertificateRef parentCertRef) {
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


    // Creates an autoreleased SecCertificateRef from a Cert object
    __unused static SecCertificateRef toSecCert(Cert *cert) {
        SecCertificateRef certRef = SecCertificateCreateWithData(kCFAllocatorDefault,
                                                            (CFDataRef)cert->data().copiedNSData());
        if (!certRef)
            throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT); // impossible?
        CFAutorelease(certRef);
        return certRef;
    }


    // Returns description of a CF object, same as "%@" formatting
    __unused static string describe(CFTypeRef ref) {
        CFStringRef desc = CFCopyDescription(ref);
        fleece::nsstring_slice s(desc);
        return string(s);
    }


#pragma mark - KEYPAIR:


#ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE     // Currently not defined for iOS

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
               LogError(TLSLogDomain, "Couldn't get the data of a public key: Not supported by macOS < 10.12 and iOS < 10.0");
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
                    // Get public key hash from kSecAttrApplicationLabel attribute:
                    // See: https://developer.apple.com/documentation/security/ksecattrapplicationlabel
                    ++gC4ExpectExceptions;
                    NSDictionary* attrs = CFBridgingRelease(SecKeyCopyAttributes(_privateKeyRef));
                    --gC4ExpectExceptions;
                    NSData* publicKeyHash = [attrs objectForKey: (id)kSecAttrApplicationLabel];
                    if (!publicKeyHash) {
                        throwMbedTLSError(MBEDTLS_ERR_X509_INVALID_FORMAT);
                    }
                    
                    // Delete Public Key:
                    NSDictionary* params = @ {
                        (id)kSecClass:                  (id)kSecClassKey,
                        (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPublic,
                        (id)kSecAttrApplicationLabel:   publicKeyHash
                    };
                    ++gC4ExpectExceptions;
                    OSStatus status = SecItemDelete((CFDictionaryRef)params);
                    --gC4ExpectExceptions;
                    if (status != errSecSuccess && status != errSecInvalidItemRef && status != errSecItemNotFound)
                        checkOSStatus(status, "SecItemDelete", "Couldn't remove a public key from the Keychain");
                    
                    // Delete Private Key:
                    params = @ {
                        (id)kSecClass:                  (id)kSecClassKey,
                        (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPrivate,
                        (id)kSecAttrApplicationLabel:   publicKeyHash
                    };
                    ++gC4ExpectExceptions;
                    status = SecItemDelete((CFDictionaryRef)params);
                    --gC4ExpectExceptions;
                    if (status != errSecSuccess && status != errSecInvalidItemRef && status != errSecItemNotFound)
                        checkOSStatus(status, "SecItemDelete", "Couldn't remove a private key from the Keychain");
                }
            } else {
                LogError(TLSLogDomain, "Couldn't remove keys: Not supported by macOS < 10.12 and iOS < 10.0");
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
                    LogTo(TLSLogDomain, "Decrypting using Keychain private key");
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
                LogError(TLSLogDomain, "Couldn't decrypt using Keychain private key: Not supported by macOS < 10.12 and iOS < 10.0");
                return MBEDTLS_ERR_RSA_UNSUPPORTED_OPERATION;
            }
        }


        virtual int _sign(int/*mbedtls_md_type_t*/ mbedDigestAlgorithm,
                          fleece::slice inputData,
                          void *outSignature) noexcept override
        {
            // No exceptions may be thrown from this function!
            if (@available(macOS 10.12, iOS 10.0, *)) {
                LogTo(TLSLogDomain, "Signing using Keychain private key");
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
                        LogWarn(TLSLogDomain, "Keychain private key: unsupported mbedTLS digest algorithm %d",
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
                LogError(TLSLogDomain, "Couldn't sign using Keychain private key: Not supported by macOS < 10.12 and iOS < 10.0");
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
            LogTo(TLSLogDomain, "Generating %d-bit RSA key-pair in Keychain", keySizeInBits);
            char timestr[100] = "LiteCore ";
            fleece::FormatISO8601Date(timestr + strlen(timestr), time(nullptr)*1000, false);
            NSDictionary* params = @ {
                (id)kSecAttrKeyType:        (id)kSecAttrKeyTypeRSA,
                (id)kSecAttrKeySizeInBits:  @(keySizeInBits),
                (id)kSecAttrIsPermanent:    @YES,
                (id)kSecAttrLabel:          @(timestr),
            };
            
            SecKeyRef publicKey = NULL, privateKey = NULL;
            if (@available(macOS 10.12, iOS 10.0, *)) {
                CFErrorRef error;
                ++gC4ExpectExceptions;
                privateKey = SecKeyCreateRandomKey((CFDictionaryRef)params, &error);
                --gC4ExpectExceptions;
                if (!privateKey) {
                    warnCFError(error, "SecKeyCreateRandomKey");
                    return nullptr;
                }
                publicKey = SecKeyCopyPublicKey(privateKey);
                
            } else {
                ++gC4ExpectExceptions;
                OSStatus err = SecKeyGeneratePair((CFDictionaryRef)params, &publicKey, &privateKey);
                --gC4ExpectExceptions;
                checkOSStatus(err, "SecKeyGeneratePair", "Couldn't create a private key");
            }
            
            return new KeychainKeyPair(keySizeInBits, publicKey, privateKey);
        }
    }

    Retained<PersistentPrivateKey> PersistentPrivateKey::withCertificate(Cert *cert) {
        if (@available(macOS 10.12, iOS 10, *)) {
            @autoreleasepool {
                SecCertificateRef certRef = toSecCert(cert);
                // Get public key from the certificate using trust:
                SecTrustRef trustRef;
                SecPolicyRef policyRef = SecPolicyCreateBasicX509();
                checkOSStatus(SecTrustCreateWithCertificates(certRef, policyRef, &trustRef),
                              "SecTrustCreateWithCertificates", "Couldn't create trust to get public key");
                SecKeyRef publicKeyRef = NULL;
                if (@available(macOS 11.0, iOS 14.0, *)) {
                    publicKeyRef = SecTrustCopyKey(trustRef);
                } else {
                    publicKeyRef = SecTrustCopyPublicKey(trustRef);
                }
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
            LogError(TLSLogDomain, "Couldn't get private key using certificate: Not supported by macOS < 10.12 and iOS < 10.0");
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
                        (id)kSecAttrKeyClass:           (id)kSecAttrKeyClassPublic
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
                LogError(TLSLogDomain, "Couldn't get private key using public key: Not supported by macOS < 10.12 and iOS < 10.0");
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
            LogTo(TLSLogDomain, "Adding a certificate chain with the id '%s' to the Keychain for '%.*s'",
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
                SecCertificateRef certRef = toSecCert(cert);
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
                
                if (status == errSecDuplicateItem && cert != this) {
                    // Ignore duplicates as it might be referenced by the other certificates
                    LogTo(TLSLogDomain, "Ignore adding the certificate to the Keychain for '%.*s' as duplicated",
                        SPLAT(cert->subjectName()));
                    continue;
                } else
                    checkOSStatus(status, "SecItemAdd", "Couldn't add a certificate to the Keychain");
                
                CFAutorelease(result);

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
                    status = SecItemUpdate((CFDictionaryRef)certQuery, (CFDictionaryRef)updatedAttrs);
                    --gC4ExpectExceptions;
                    if (status != errSecSuccess) {
                        // Rollback by deleteing the added certificate:
                        ++gC4ExpectExceptions;
                        OSStatus deleteStatus = SecItemDelete((CFDictionaryRef)certQuery);
                        --gC4ExpectExceptions;
                        if (deleteStatus != errSecSuccess) {
                            warnOSStatusError(deleteStatus, "SecItemDelete",
                                              "Couldn't delete certificate that was failed to update the label.");
                        }
                    }
                    checkOSStatus(status, "SecItemUpdate",
                                  "Couldn't update the label to a certificate in Keychain");
                }
            #endif
                
                if (!entireChain)
                    break;
            }
        }
    }


    fleece::Retained<Cert> Cert::loadCert(const std::string &persistentID) {
        @autoreleasepool {
            LogTo(TLSLogDomain, "Loading a certificate chain with the id '%s' from the Keychain", persistentID.c_str());
            
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
            OSStatus err;
            if (@available(iOS 12.0, macos 10.14, *)) {
                CFErrorRef cferr;
                if (!SecTrustEvaluateWithError(trustRef, &cferr)) {
                    auto error = (__bridge NSError*)cferr;
                    LogVerbose(TLSLogDomain, "SecTrustEvaluateWithError failed: %s", error.description.UTF8String);
                }
                err = SecTrustGetTrustResult(trustRef, &result);
            } else {
#if TARGET_OS_MACCATALYST
                LogError(TLSLogDomain, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14, iOS < 12");
                error::_throw(error::UnsupportedOperation, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14, iOS < 12");
#else
                err = SecTrustEvaluate(trustRef, &result);
#endif
            }
            checkOSStatus(err, "SecTrustEvaluate",
                          "Couldn't evaluate the trust to get certificate chain" );
            
            CFIndex count = SecTrustGetCertificateCount(trustRef);
            Assert(count > 0);
            if (@available(macOS 12.0, iOS 15.0, *)) {
                CFArrayRef certs = SecTrustCopyCertificateChain(trustRef);
                for (CFIndex i = 1; i < count; i++) {
                    SecCertificateRef ref = (SecCertificateRef)CFArrayGetValueAtIndex(certs, i);
                    NSData* data = (NSData*) CFBridgingRelease(SecCertificateCopyData(ref));
                    cert->append(new Cert(slice(data)));
                }
                CFRelease(certs);
            } else {
                for (CFIndex i = 1; i < count; i++) {
                    SecCertificateRef ref = SecTrustGetCertificateAtIndex(trustRef, i);
                    NSData* data = (NSData*) CFBridgingRelease(SecCertificateCopyData(ref));
                    cert->append(new Cert(slice(data)));
                }
            }
            
            return cert;
        }
    }

    void Cert::deleteCert(const std::string &persistentID) {
        @autoreleasepool {
            LogTo(TLSLogDomain, "Deleting a certificate chain with the id '%s' from the Keychain",
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
            OSStatus err;
            if (@available(iOS 12.0, macos 10.14, *)) {
                CFErrorRef cferr;
                if (!SecTrustEvaluateWithError(trustRef, &cferr)) {
                    auto error = (__bridge NSError*)cferr;
                    LogVerbose(TLSLogDomain, "SecTrustEvaluateWithError failed: %s", error.description.UTF8String);
                }
                err = SecTrustGetTrustResult(trustRef, &result);
            } else {
#if TARGET_OS_MACCATALYST
                LogError(TLSLogDomain, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14 and iOS < 12");
                error::_throw(error::UnsupportedOperation, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14 and iOS < 12");
#else
                err = SecTrustEvaluate(trustRef, &result);
#endif
            }

            checkOSStatus(err, "SecTrustEvaluate",
                          "Couldn't evaluate the trust to get certificate chain");
            
            CFIndex count = SecTrustGetCertificateCount(trustRef);
            Assert(count > 0);
            if (count == 1) {
                NSDictionary* params = @{
                    (id)kSecClass:              (id)kSecClassCertificate,
                    (id)kSecValueRef:           (__bridge id)certRef
                };
                checkOSStatus(SecItemDelete((CFDictionaryRef)params),
                              "SecItemDelete",
                              "Couldn't delete a certificate from the Keychain");
                return;
            }
            
            if (@available(macOS 12.0, iOS 15.0, *)) {
                CFArrayRef certs = SecTrustCopyCertificateChain(trustRef);
                for (CFIndex i = count - 1; i >= 0; i--) {
                    SecCertificateRef copiedRef = (SecCertificateRef)CFArrayGetValueAtIndex(certs, i);
                    if (getChildCertCount(copiedRef) < 2) {
                        // Cert copied cannot be used directly to delete, so we will use the primary
                        // key: issuer + serial-num + cert-type
                        NSDictionary* attrs = CFBridgingRelease(findInKeychain(@{
                            (id)kSecClass:              (id)kSecClassCertificate,
                            (id)kSecValueRef:           (__bridge id)copiedRef,
                            (id)kSecReturnAttributes:   @YES
                        }));
                        
                        NSData* issuer = [attrs objectForKey: (id)kSecAttrIssuer];
                        Assert(issuer);
                        NSData* serialNum = [attrs objectForKey: (id)kSecAttrSerialNumber];
                        Assert(serialNum);
                        NSNumber* certType = [attrs objectForKey: (id)kSecAttrCertificateType];
                        Assert(certType != nil);
                        
                        NSDictionary* params = @{
                            (id)kSecClass:                  (__bridge id)kSecClassCertificate,
                            (id)kSecAttrCertificateType:    certType,
                            (id)kSecAttrIssuer:             issuer,
                            (id)kSecAttrSerialNumber:       serialNum,
                        };
                        checkOSStatus(SecItemDelete((CFDictionaryRef)params),
                                      "SecItemDelete",
                                      "Couldn't delete a certificate from the Keychain");
                    }
                }
                CFRelease(certs);
            } else {
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

#endif // PERSISTENT_PRIVATE_KEY_AVAILABLE


#ifdef ROOT_CERT_LOOKUP_AVAILABLE

    fleece::Retained<Cert> Cert::findSigningRootCert() {
        @autoreleasepool {
            auto policy = SecPolicyCreateBasicX509();
            
            // Create trust with a cert chain including all intermediates:
            NSMutableArray* certChain = [NSMutableArray array];
            for (Retained<Cert> crt = this; crt; crt = crt->next()) {
                [certChain addObject: (__bridge id)(toSecCert(crt))];
            }
            
            LogTo(TLSLogDomain, "findSigningRootCert: Evaluating %s ...",
                  describe((__bridge SecCertificateRef)certChain.firstObject).c_str());
            
            SecTrustRef trust;
            checkOSStatus(SecTrustCreateWithCertificates((__bridge CFArrayRef)certChain, policy, &trust),
                          "SecTrustCreateWithCertificates", "Couldn't validate certificate");
            CFAutorelease(policy);
            CFAutorelease(trust);
            
            SecTrustResultType result;
            OSStatus err;

            if (@available(iOS 12.0, macos 10.14, *)) {
                CFErrorRef cferr;
                if (!SecTrustEvaluateWithError(trust, &cferr)) {
                    auto error = (__bridge NSError*)cferr;
                    LogVerbose(TLSLogDomain, "SecTrustEvaluateWithError failed: %s", error.description.UTF8String);
                }
                err = SecTrustGetTrustResult(trust, &result);
            } else {
#if TARGET_OS_MACCATALYST
                LogError(TLSLogDomain, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14, iOS < 12.0");
                error::_throw(error::UnsupportedOperation, "Catalyst:SecTrustEvaluateWithError not available, macOS < 10.14, iOS < 12.0");
#else
                err = SecTrustEvaluate(trust, &result);
#endif
            }
            checkOSStatus(err, "SecTrustEvaluate", "Couldn't validate certificate");
            LogTo(TLSLogDomain, "    ...SecTrustEvaluate returned %d", result);
            if (result != kSecTrustResultUnspecified && result != kSecTrustResultProceed)
                return nullptr;
            
            Retained<Cert> root;
            CFIndex certCount = SecTrustGetCertificateCount(trust);
            if (@available(macOS 12.0, iOS 15.0, *)) {
                CFArrayRef certs = SecTrustCopyCertificateChain(trust);
                for (CFIndex i = 1; i < certCount; ++i) {
                    SecCertificateRef certRef = (SecCertificateRef)CFArrayGetValueAtIndex(certs, i);
                    LogTo(TLSLogDomain, "    ... root %s", describe(certRef).c_str());
                    CFDataRef dataRef = SecCertificateCopyData(certRef);
                    CFAutorelease(dataRef);
                    Retained<Cert> cert = new Cert(slice(dataRef));
                    if (root == nil)
                        root = cert;
                    else
                        root->append(cert);
                }
                CFRelease(certs);
            } else {
                for (CFIndex i = 1; i < certCount; ++i) {
                    auto certRef = SecTrustGetCertificateAtIndex(trust, i);
                    LogTo(TLSLogDomain, "    ... root %s", describe(certRef).c_str());
                    CFDataRef dataRef = SecCertificateCopyData(certRef);
                    CFAutorelease(dataRef);
                    Retained<Cert> cert = new Cert(slice(dataRef));
                    if (root == nil)
                        root = cert;
                    else
                        root->append(cert);
                }
            }
            return root;
        }
    }

#endif // ROOT_CERT_LOOKUP_AVAILABLE


} }
