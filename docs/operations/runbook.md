# Operations And Recovery Runbook

Health covers state availability and required local files. Runtime status is
bounded to profile, agent ID, balance, storage byte count, active task count,
configuration revision, and dependency state. It excludes content, attachment
bytes, private keys, signatures, and message history.

Use a correlation ID across intake, storage, payment, and receipt events.
`RedactedTelemetry` recursively removes sensitive fields. Circuit breakers open
after a configured consecutive-failure threshold and permit a retry only after
cooldown. Bounded queues reject new work instead of consuming unbounded memory.

## Recovery

1. Stop the affected instance without deleting its data directory.
2. Run `health`, then preserve a `backup` before repair.
3. Verify free disk space, module checksum, network, and dependency reachability.
4. Restart using the same profile and data directory.
5. Inspect reconciliation counts for outbox records, bonds, and active A2A tasks.
6. Replay only idempotent events; never create a replacement bond or payment ID.
7. Confirm private receipts and final program state before closing an incident.

For corrupted JSON configuration, restore a known backup. For a corrupted
SQLite database, retain the file for investigation and restore; do not silently
recreate state while bonds are outstanding. Owner unavailability must leave
approvals pending until timeout, then expired.
