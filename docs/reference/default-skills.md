# Default Skill Reference

The canonical runtime catalog is `src/runtime/default_skill_catalog.cpp`.
`meta.skills` emits the profile-filtered schemas directly from registered
definitions. The conformance suite asserts that the union contains exactly the
21 operations below.

| Skill | Inbox | Vault | Settlement | Behavior |
|---|:---:|:---:|:---:|---|
| `storage.upload` | no | yes | no | Encrypt and store content; return commitment metadata. |
| `storage.download` | no | yes | no | Fetch, authenticate, and decrypt a known object. |
| `storage.list` | no | yes | no | List local metadata, never plaintext. |
| `storage.share` | no | yes | no | Issue an expiring recipient grant. |
| `messaging.send` | yes | no | no | Seal to `recipient_encryption_public_key`, sign, and send a versioned envelope. |
| `messaging.join` | yes | no | no | Join a group by identifier. |
| `messaging.create_group` | yes | no | no | Create a group for explicit members. |
| `wallet.balance` | no | no | yes | Return LEZ balance. |
| `wallet.send` | no | no | yes | Execute within limits or return an approval proposal. |
| `wallet.history` | no | no | yes | Return transfer metadata. |
| `program.query` | no | no | yes | Query a program without mutation. |
| `program.call` | no | no | yes | Invoke a program instruction. |
| `program.deploy` | no | no | yes | Deploy an existing program binary. |
| `agent.card` | no | yes | no | Get or publish a signed `lf.a2a.v1` card. |
| `agent.discover` | no | yes | no | Find valid, unexpired cards by skill. |
| `agent.task` | no | yes | no | Create or advance a paid task. |
| `agent.subscribe` | no | yes | no | Read current task state and revision. |
| `agent.cancel` | no | yes | no | Cancel an active task and request escrow refund. |
| `meta.skills` | yes | yes | yes | Return the allowed runtime manifest. |
| `meta.status` | yes | yes | yes | Return redacted health and resource status. |
| `meta.configure` | yes | yes | yes | Apply an owner-signed revision-checked update. |

All inputs and outputs are JSON objects. Unknown skills, disallowed profiles,
invalid schemas at service boundaries, and handler exceptions fail the request
without changing registry state.
