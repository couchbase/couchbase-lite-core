# Signed JSON Objects and Documents

January 19, 2022

This is a specification for digitally signing a JSON object, intended for use with document databases like Couchbase.

## 1. Introduction

Signing a document provides these benefits:
* The enclosed public key can be used as an identifier of the entity that signed the document.
* Any unauthorized modification of the document can be detected, as it'll invalidate the signature.

Thus a signature serves as a form of authentication. Why do we need this when servers like Sync Gateway already support several types of authentication?
* The signature authenticates a _document_, not a _connection_. This is a very important distinction when documents are replicated, especially when they can pass between multiple servers. A document may be forwarded by an entity that didn't create it, so the fact that the replicator connection is authenticated does _not_ authenticate the document. The document has to carry its own credentials.
* Public keys allow for many types of identity and authentication. In the simplest case, an entity can create a key-pair and use the public key as its sole identification; this is useful even though it doesn't tie that entity to any external form of ID. More complex systems can use a hierarchical public-key infrastructure like X.509 or a "web of trust" like PGP.

**History:**

* 2009: Initial draft suggested to the CouchDB community. Heavily influenced by [SDSI](http://groups.csail.mit.edu/cis/sdsi.html), an experimental public-key infrastructure that used S-expressions as its universal data representation.
* Feb/March 2014: Significant evolution/rewrite of the old spec. First appearance in the Couchbase Lite wiki.
* August 2014: Simplified the data format by removing the use of nested arrays tagged with algorithm IDs. Added support for Curve25519 algorithm.
* July 2015: Replaced references to Curve25519 with Ed25519, since the latter is what's actually used for signatures (Curve25519 itself only does encryption.)
* January 2022: Revised for Couchbase Lite Core. Changed various property names. Changed canonical-JSON description to match the reality of what Fleece’s JSON encoder does.

## 2. Usage

There are two main algorithms here: the **signature algorithm** takes a JSON object and a private key and produces a signature object, and the **verification algorithm** takes a JSON object and a signature object and determines whether or not the signature is valid for that object.

> **Note:** These algorithms will work with objects in any JSON-equivalent data format or API, such as Couchbase’s Fleece. The only time they require actual literal JSON is in the production of a canonical encoding, but that canonical JSON never appears anywhere, it just disappears into a SHA digest.

Unlike some other JSON-signature systems, the object being signed doesn't need to be specially encoded. This is important because it doesn't get in the way of applications or middleware that read and write the objects.

Another advantage is that the signature doesn't need to be contained in the signed object (or vice versa.) It is common for the signature to be contained -- and there's a special `(sig)` property defined for it -- but there are situations where this isn't practical. In this case it's up to the application to have a way to find the signature of an object.

## 3. Data Formats

### Cryptographic Algorithms and Formats

This spec uses the SHA family of digest algorithms, and RSA or [Ed25519](http://ed25519.cr.yp.to/) signatures. Where digests and signatures appear in object properties, they are base64-encoded and their types are identified by adding a suffix to the property name. The suffixes are `SHA`, `RSA`, and `Ed25519`.

For example, a SHA-256 digest looks like:

```json
"digest_SHA": "0yiour/fLeTxyK2O5nOjRt8PwYbX/R/oq27/y5vtfcA="
```

Notes:

* SHA and RSA have multiple key or output sizes; the size doesn't need to be given explicitly in the property name because it can easily be inferred from the length of the associated binary data.

* RSA signatures are produced from a SHA-256 digest of the input data. They are the same size as the key length (e.g. 2048 bits = 256 bytes.)

* RSA public keys have multiple binary representations. Use the BSAFE (ASN.1 DER) format.
* Ed25519 signatures are produced from a SHA-512 digest of the input data. They are 64 bytes long.

* Ed25519 public keys are 32 bytes.

Other algorithms could be added in the future; they just need standard suffix strings.

### Signature Object

This is a JSON object that acts as a digital signature of _some other JSON object_ (without specifying where that other object is.)

Absolutely-required properties, each of which has a base64 string as its value:

* `digest_SHA`: A cryptographic digest of the canonical JSON encoding of the object being signed.
* `sig_RSA` or `sig_Ed25519`: The digital signature of the canonical encoding of the signature object _minus this field_.

Usually-required properties that will be present in most use cases:

* `key`: The public key of the key-pair performing the signing. (The algorithm doesn't need to be specified: it's the same as the one named by the `sig_xx` property.) The key could be omitted if the the verifying party will know the signer’s public key through some other means. For example, the document might include an identifier that can be used to look it up.
* `date`: A timestamp identifying when the signature was generated. May be an ISO-8601-format string (example: `"2014-08-29T16:22:28Z"`), or an integer denoting the number of milliseconds since the Unix epoch Jan 1 1970 00:00AM UTC.

* `expires`: The number of **minutes** the signature remains valid after being generated.

### Signed Object

This is simply a JSON object that directly contains its signature as the value of a `(sig)` property. Obviously this property needs to be ignored while computing the canonical digest of the object.

## 4. Algorithms

### Canonical Digests of JSON (compatible) Objects

Digest algorithms like SHA operate on raw binary data, not abstract objects like JSON or Fleece. Unfortunately a JSON value can be encoded in many slightly different ways; you can change the order of object keys, escape different characters in strings, add or omit whitespace. All of those differences will result in different digests. 

So for the signer and verifier to agree on the same digest of an object, there has to be a canonical encoding algorithm that always maps equivalent objects to identical data.

Therefore the algorithm to create a digest of JSON is:

1. (Re-)encode the object in canonical JSON encoding (q.v.).
2. Compute a digest of the UTF-8 byte-string produced.

### Canonical JSON Encoding

There have been various rules proposed for canonicalizing JSON, and of course they don’t all agree. The differences are mostly in the handling of strings and object keys. Here are the rules we use:

* No whitespace.
* Numbers should be integers in the range [-2^47^ .. 2^47^-1].
* Numbers cannot be encoded with decimal points, scientific notation, or leading zeros. "`-0`" is not allowed either. (`NaN` and `Inf` are right out — they’re not valid JSON at all.)
* Strings (including keys) are converted to [Unicode Normalization Form C](http://www.unicode.org/reports/tr15/).
* Strings use the escape sequences `\\`, `\"`, `\r`, `\n`, `\t`. Other control characters (00-1F and 7F) are escaped using `\uxxxx`. All other characters are written literally as UTF-8, including non-ASCII ones.
* Object keys are lexicographically sorted by their UTF-8 string representation, i.e. as by `strcmp`.
* The output text encoding is of course UTF-8.

Non-integers are strongly discouraged because different formatting libraries will convert them to decimal form in different ways (e.g. `1.1` vs `1.099999`). Even integers are limited to 48-bit, not 64-bit, because many JSON parsers convert all numbers to double-precision floating point, which is a 64-bit value but only has about 50 bits of precision (mantissa), and drops the least significant bits of integers outside ~ ±2^50^.

> **Note:** The Fleece library’s JSON encoder produces this encoding when its `canonical` parameter is `true`. It does not reject invalid numbers, just encodes them as well as it can. Thus, if you sign an object containing invalid numbers, it’s pretty likely it will validate correctly in any other system using Fleece (i.e. Couchbase Lite). But other software implementing these algorithms may not encode the numbers exactly the same, which would break verification.

### Creating A JSON Signature Object

1. Compute the SHA-256 digest of the canonical JSON of the object being signed.
2. Create an empty object to hold the signature, and store a base64 string of the digest in its `digest_SHA` property.
2. Store a base64 string of the public key in the `key` property (unless the key will already be known to the verifier and you want to save room by omitting it from the signature.)
3. Store the current date/time in the `date` property, as an ISO-8601 string or a milliseconds-since-Unix-epoch integer.
3. Store the duration of validity, in minutes, as an integer in the `expires` property.
4. Add any other optional properties desired. (See *Usage With Document Databases*, below.)
5. Compute the canonical JSON of the signature object you just constructed.
6. Generate a digital signature of that canonical JSON, using the private key that matches the public key used in step 2. (This will involve computing a digest and then signing the digest; some crypto APIs make that explicit, and some do it for you.)
7. Add that signature, base64-encoded, as the `sig_RSA` or `sig_Ed25519` property of the signature object.

### Verifying A JSON Signature Object

1. Temporarily remove from the target object (the one the signature signed) any properties that weren’t present when it was signed; for instance, the `(sig)` property itself, if the object contains its own signature.
1. Compute the canonical digest of the target object, using the algorithm given in the signature object's `digest` property.
2. Compare this digest with the one contained in the `digest` property of the signature. If they aren't equal, **fail (the object does not match what was signed.)**
3. Copy the signature object and remove the `sig` property from the copy.
4. Compute the canonical digest of the copied signature object.
5. Verify the digest against the signature contained in the `sig` property, using the public key contained in the `key` property. If verification fails, **fail (the signature object itself has been altered.)**
7. If the signature contains a `date` property:
   1. Decode the date as an ISO-8601 or integer timestamp. If that date is more than one minute in the future, **fail (not valid yet, or else there's unacceptable clock skew.)**
   2. There must be an `expires` property, containing a positive integer. Add that number of minutes to the `date`. If the resulting time is in the past, **fail (signature expired).**

8. **Succeed: the signature is valid!**

At any step:
* If any value in the signature object is invalid (invalid ISO-8601, invalid base64, etc.), then **fail (the signature is syntactically invalid).**
* If any algorithm string is unrecognized or the program can't perform that algorithm, then **fail (not possible to verify the signature.)** It is not known whether the signature is valid, but the application should _not_ trust the signature or the object that was signed.

## 5. Usage With Document Databases

When signing documents belonging to a Couchbase database it's important to handle the document metadata correctly.

The key point is that **the document ID and revision ID must be included in the signature**. If not, the document and signature can be used for replay attacks. If the doc ID isn't signed, an attacker can create a copy of the signed document under a different ID. If signature doesn't include the revision ID, an attacker can re-post the signed revision at any time, reverting the document to an older version.

Note: To the verifier, the revision ID in the signature is actually the *parent* rev ID of the signed revision. It was current at the time the revision was being signed. (The new revision’s ID isn’t known until after it’s been saved, which is too late to change anything in it!)


Specifically:

* The document ID must be added to the signature as a `docID` property.
* The current revisionID at the time the new revision is being signed must be added to the signature as a `parentRev` property, unless this is a first-generation document with no parent revision.

## 6. Example

A signature object using Ed25519, created January 18, 2022 at 12:52 PST with a five-minute expiration time:
```json
{
  "digest_SHA": "0yiour/fLeTxyK2O5nOjRt8PwYbX/R/oq27/y5vtfcA=",
  "key": "RjhO2DQvPfa5A+YtpCYHxg0jajjfyLIAryANpe/MxCA=",
  "sig_Ed25519": "pvr9sLAjEJx+D6DfE0kjwO+gbcI5WUgaZTiDvliddXfGRbALeo1tcppPmsGDujN3ZoEojVk7g1BykgVR3kM+AA==",
  "date": 1642632165223,
  "expires": 5
}
```

The same signature, embedded in the document it signs:
```json
{
  "age": 6,
  "name": "Oliver Bolliver Butz",
  "(sig)": {
    "digest_SHA": "0yiour/fLeTxyK2O5nOjRt8PwYbX/R/oq27/y5vtfcA=",
    "key": "RjhO2DQvPfa5A+YtpCYHxg0jajjfyLIAryANpe/MxCA=",
    "sig_Ed25519": "pvr9sLAjEJx+D6DfE0kjwO+gbcI5WUgaZTiDvliddXfGRbALeo1tcppPmsGDujN3ZoEojVk7g1BykgVR3kM+AA==",
    "date": 1642632165223,
    "expires": 5
  }
}
```