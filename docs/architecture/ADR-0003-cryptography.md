# ADR-0003: Audited Cryptography and Domain Separation

**Status:** accepted

The local implementation uses OpenSSL EVP primitives: Ed25519 for signatures,
SHA-256 for commitments, HKDF-SHA-256 for derived keys, and AES-256-GCM for
authenticated local encryption. Production identity keys must come from the
verified Logos/LEZ key provider rather than application-generated files.

Every signed object includes protocol, network, object type, identity, policy
version, identifier, nonce, and expiry as applicable. Canonical JSON is sorted
before hashing or signing. No custom cryptographic primitive is permitted.
