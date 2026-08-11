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

Wallet spending is serialized by `SpendingController`. An amount within both
transaction and rolling-period limits executes once. Anything above either
limit becomes a bounded-lifetime approval proposal. Expired, denied, duplicate,
and conflicting decisions fail closed.
