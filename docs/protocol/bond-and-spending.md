# Bond And Spending Protocol

An inbox policy signs its owner, fixed non-owner sink, bond amount, response
deadline, attachment limits, network, and version. A submission commits to the
policy hash and binds its escrow as `bond:<message-id>`.

- Accepted, expired, and delivery-failed messages refund the sender in full.
- Rejection sends the bond only to the policy sink.
- Classifier output is advisory and cannot authorize rejection.
- Automatic rejection requires an enumerated, reproducible policy violation.
- Model-based rejection requires signed owner intent covering message, policy,
  sink, and timestamp.

The LEZ guest stores only bond IDs, message and policy commitments, account IDs,
amount, deadline, and terminal outcome. Message content and attachments are never
instruction or account data. Its public ABI has two instructions:

- `Initialize` validates the signed sender, fixed owner/sink,
  state and escrow PDAs, then chains the exact amount into an
  authenticated-transfer-owned escrow. Its proof output is valid only before
  the committed deadline according to the sequencer block timestamp.
- `Settle` validates the recorded state and exact escrow balance, permits expiry
  only through a proof output valid from the committed deadline, requires owner
  authorization for every other outcome, and chains the full amount to the
  recorded sender or sink.

The mutable one-block clock account is deliberately excluded from private-proof
inputs. Real proof construction can take hours; committing a clock pre-state
would make the proof stale before submission. LEZ applies the proof's timestamp
validity window against the block timestamp instead.

A terminal outcome is persisted before transfer, so a repeated settlement fails.
The guest is pinned to the exact official LEZ testnet ABI documented in the
upstream compatibility matrix.

Wallet spending is serialized by `SpendingController`. An amount within both
transaction and rolling-period limits executes once. Anything above either
limit becomes a bounded-lifetime approval proposal. Expired, denied, duplicate,
and conflicting decisions fail closed.
