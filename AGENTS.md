# Workspace instructions

## Work locations

- Perform ECU-related work inside the `ecu/` directory.
- Perform IDS-related work inside the `ids/` directory.

## Required context before work

1. Read `./project.md` completely before designing, implementing, testing, reviewing, or documenting this project.
2. Under `./docs/check`, the only always-designated source set is:
   - `R155e (2).docx`
   - `R155am1e.docx`
   - `R155am2e.docx`
   - `R155am3e.docx`
   - `R156e (2).docx`
   - `ISOSAE_21434_기반_자동차_사이버보안_테스트_및_검증_매뉴얼.pdf`
3. Confirm those six files and their revisions, then read the clauses or sections relevant to the task. Treat the R155 base document and Amendments 1 through 3 as one cumulative source set.
4. Files under `docs/evidence`, `docs/results`, `docs/superpowers`, and other `docs` paths are required only when they are relevant to the current phase, changed area, or verification claim.

## Missing decisions

For missing information or configuration, first use regulations, standards, official public automotive implementations, and real-vehicle role and trust boundaries to identify the most vehicle-like supportable setting. Prefer that setting over a development-convenience shortcut, record its basis in `project.md`, and keep the SIL claim boundary explicit.

If more than one realistic vehicle architecture remains, the value is OEM-specific or unavailable, or the choice can materially affect architecture, protocol behavior, IDs, addresses, ports, security boundaries, permissions, limits, data formats, acceptance criteria, evidence, or claims, stop before making dependent changes and ask the user.

State the missing item, why it is needed now, the affected work, realistic options, and a recommendation. Do not silently invent a temporary value, OEM requirement, or default. Continue only work that is independent of the unresolved decision.

Use `project.md` as the implementation source of truth. If it conflicts with the user's current direction, a required source document, or observed implementation and test evidence, surface the conflict and get a decision before proceeding.
