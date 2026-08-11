# Headless Deployment

Prerequisites are a pinned `bonded_inbox.lgx`, an executable Logos Core binary,
an owner Ed25519 public key, and a writable private data directory.

```bash
bin/bonded-inbox --data-dir /var/lib/bonded-inbox plan \
  --profile inbox --network lez-testnet \
  --owner-public-key OWNER_PUBLIC_KEY \
  --module ./result/logos-bonded_inbox-module-lib.lgx \
  --core-binary /opt/logos/bin/logos-core

bin/bonded-inbox --data-dir /var/lib/bonded-inbox deploy \
  --profile inbox --network lez-testnet \
  --owner-public-key OWNER_PUBLIC_KEY \
  --module ./result/logos-bonded_inbox-module-lib.lgx \
  --core-binary /opt/logos/bin/logos-core
```

The command validates inputs, creates a mode-`0700` directory, generates an
independent local identity, writes configuration atomically, records the module
SHA-256, and emits one JSON result. Repeating the same command is a no-op;
different configuration requires `upgrade`.

The CLI does not download binaries, expose an HTTP API, print the ISK, or claim
that writing a service descriptor started Logos Core. An operator or packaging
system must install and start the generated `service.json` command.

Use separate directories and identities for `inbox`, `vault`, and `settlement`.
Run `status`, `health`, `logs`, `policy`, `fund`, `approve`, `deny`, `backup`,
`restore`, `upgrade`, and `rollback` as documented by `--help`. Teardown only
accepts a deployment created with `--test-deployment` and the exact confirmation
string.
