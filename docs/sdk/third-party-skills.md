# Third-Party Skill SDK

Third-party handlers use runtime API `bonded.skill/v1` and register a
`SkillDefinition` containing a stable dotted name, description, JSON input and
output schemas, profile allowlist, and callable handler.

1. Start from `examples/external-skill/skill.json`.
2. Keep parsing bounded and treat every input as untrusted.
3. Request only the profiles and adapter capabilities the skill needs.
4. Never accept private keys, raw wallet secrets, or owner authorization tokens.
5. Throw `DomainError` for a safe request failure; do not terminate the process.
6. Add conformance tests for schema limits, profile denial, timeout, and failure
   isolation.

The runtime test registers one failing external handler followed by a healthy
handler and proves the second remains callable. Dynamic shared-library loading
is intentionally disabled until Logos Core publishes a stable third-party ABI;
packages are currently linked by the module owner without edits to registry
core.
