# ADR-0004: A2A over Logos Messaging

**Status:** accepted

The project pins the `lf.a2a.v1` protobuf contract. Agent Cards and task messages
retain A2A fields and state meanings; a signed Bonded transport envelope adds
the Logos topic, sender identity, replay nonce, expiration, and optional LEZ
payment commitment. HTTP endpoints are not exposed by the agent runtime.

Payment acceptance and task state are coordinated but persisted independently.
A task cannot become paid-completed until both its result and confirmed payment
are recorded. Cancellation is idempotent and follows the advertised refund
policy.
