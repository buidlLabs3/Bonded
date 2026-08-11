# External Skill Example

`skill.json` is the minimal package descriptor for runtime API
`bonded.skill/v1`. A host registers the handler as a `SkillDefinition`; the
profile allowlist, input/output schemas, and exception boundary are enforced by
`SkillRegistry`. See `docs/sdk/third-party-skills.md`.

The sample intentionally has no network or wallet permission. A handler failure
is returned as a request failure and does not remove or mutate other skills.
