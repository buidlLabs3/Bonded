# Owner Guide

The owner uses an encrypted Logos Messaging channel or the Basecamp page; no
public administration API is required.

For each pending message, verify the committed policy version, sender identity,
bond amount, attachment metadata, deadline, local classifier reasons, and any
deterministic evidence. Acceptance refunds the sender. Before rejection,
Basecamp displays the fixed community sink and requires explicit confirmation.
The owner cannot select themselves as the sink.

Above-limit spending appears in Approvals with recipient, LEZ amount, and
expiry. Approve only the exact intended proposal. Unanswered proposals expire
without execution. Settings publication is signed and revision-checked;
conflicting updates must be refreshed and reviewed again.

Offline and reconnecting states do not authorize actions. After recovery,
refresh status and reconcile pending messages, tasks, approvals, bonds, and
receipts before taking a terminal decision.

## Local Basecamp Preview

The repository includes a Qt 6 preview with non-secret fixture data. It is a
local interaction test for the same `BondedInboxPage` packaged into the
Basecamp `.lgx`; it is not connected to an owner identity or LEZ testnet.

```bash
BONDED_QML_RUNNER=/absolute/path/to/qmlscene scripts/run-basecamp-preview.sh
```

Use the four tabs to exercise message acceptance and rejection, task state,
approval decisions, and settings publication. The footer reports the action
handled by the preview fixture. Closing the window ends the preview.

For a non-interactive instantiation check:

```bash
QT_QPA_PLATFORM=offscreen BONDED_QML_RUNNER=/absolute/path/to/qmlscene \
  scripts/run-basecamp-preview.sh --smoke
```
