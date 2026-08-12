# Privacy And Economic Abuse

Only commitments, amounts, destinations, program state, and transaction/proof
references belong on-chain. Content, attachment bytes, contact lists, classifier
input, keys, and private receipt history remain encrypted or local. Telemetry
redacts payload/content/private/secret/attachment/key/signature fields.
Owner-channel v2 messages use a fresh ephemeral X25519 key and context-bound
AES-256-GCM sealing before the complete wire envelope is signed with Ed25519;
the transport schema contains no plaintext payload field.

Spam and Sybil pressure are constrained by per-sender windows, exact bonds,
attachment limits, and bounded queues. Trusted bypass requires an authenticated,
revocable identity. A bond cannot profit the owner. Underpayment, stale policy,
wrong bond ID, price mismatch, replay, conflicting settlement, and cancellation
races fail.

Residual risks include low-bond spam economics, owner censorship, unavailable
owners, endpoint compromise, metadata leakage, storage availability, and fee/CU
changes. Operators should keep a bond-free emergency channel, choose a small
but meaningful bond, publish a community-controlled sink, use separate profile
identities, cap task prices, back up state, and avoid production funds.
