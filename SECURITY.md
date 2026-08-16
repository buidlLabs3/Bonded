# Security Policy

Bonded Inbox handles private messages and testnet financial state. Report
vulnerabilities through GitHub private vulnerability reporting, not a public
issue, when keys, plaintext, identities, or funds may be exposed.

Never include credentials, recovery phrases, private keys, wallet storage,
private messages, raw proofs, or complete wallet history in reports or evidence.
Use disposable fixtures and include the affected commit, impact, and minimal
reproduction.

Wallet directories must remain outside the repository with mode `0700`; wallet
files and signer keys must be mode `0600`. Message content and attachment bytes
never belong on-chain. A model result cannot confiscate a bond: rejection needs
an explicit owner decision or a deterministic signed policy rule.

Paid Agent Cards contain only LEZ public receiving material (account ID, NPK,
and VPK). Wallet recovery material, spending keys, and wallet files are never
published or sent over the A2A channel.

The project is experimental testnet software and is not approved for production
funds or identities. Only the latest tagged release is supported.
