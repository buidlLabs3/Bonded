# ADR-0002: Transactional State and Terminal Settlement

**Status:** accepted

SQLite stores workflow metadata, policy snapshots, idempotency records, and an
outbox in one transaction. Sensitive payloads are encrypted before persistence.
Attachment ciphertext belongs in Logos Storage; plaintext exists only in bounded
temporary processing buffers.

Bond settlement is a monotonic state machine. Accepted, rejected, expired, and
delivery-failed are terminal and mutually exclusive. Acceptance, expiry, and
failed delivery refund the sender. Rejection transfers only to the signed
policy's fixed sink and can never benefit the owner.
