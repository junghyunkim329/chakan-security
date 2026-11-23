# Boost package integrity implementation plan

Spec: ../specs/2026-11-23-boost-package-integrity-design.md

Use the existing checkout, preserve unrelated edits, and implement inline as authorized.

- [x] Add executable regression tests using real Debian packages: valid input, tampering, extra/missing packages, metadata mismatch, malformed manifest, existing-root preservation.
- [x] Observe missing-enforcement assertion failures before implementation (15 fixture failures; not a runtime exploit of the old script).
- [x] Add a strict manifest parser, exact-set/size/SHA-256/control-field verifier, private download staging, explicit extraction and atomic root exchange.
- [x] Move the build implementation under ECU, retain the root entry point, and invoke preparation on every build.
- [x] Add the regression suite to CTest and record automotive sources and claim boundaries in project.md.
- [x] Run focused tests, real pinned package verification, shell syntax, pytest and CTest; review the diff and report any limits.

Review focus: symlinks, duplicate/path-traversal manifest entries, cached-root bypass, failed preparation, and unexpected packages.

Verification: 17 focused cases; repository pytest 35 passed, 2 conditional skips, 11 subtests; CTest 16/16; project build succeeded. Actual Ubuntu APT download of all five pinned packages, manifest verification and extraction succeeded in a private /tmp directory after sandbox DNS failure and approved retry. Existing --status entry point works. Initial CMake Ninja request conflicted with the existing Unix Makefiles cache; rerun with the existing generator succeeded. The user subsequently ran the production entry point: the on-disk log confirms five approved packages verified, 236/236 vSomeIP compilation steps, installation completed at 2026-11-23 18:57:11 +09:00, and state `done`.

Independent read-only review found no direct verification bypass in the trusted-host/build-user model. Review identified archive timestamps permitting old vSomeIP objects: added `--clean-first`. Shell also holds the preparer's common lock while compiling to prevent standalone preparation overlapping compilation. No unrelated working-tree changes were removed or committed.
